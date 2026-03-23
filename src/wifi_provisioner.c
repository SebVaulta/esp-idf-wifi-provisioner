/*
 * SPDX-FileCopyrightText: 2026 Michael Teeuw
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Main orchestration: boot flow, connect-or-provision logic.
 */

#include "wifi_prov_internal.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"

/* Use the project's embedded CA certificate (if present) for TLS verification. */
extern const uint8_t certificate_pem_start[] asm("_binary_certificate_pem_start");
extern const uint8_t certificate_pem_end[]   asm("_binary_certificate_pem_end");

#define CONNECTED_BIT BIT0

static const char *TAG = "wifi_prov";

static wifi_prov_config_t s_config;
static esp_netif_t       *s_sta_netif = NULL;
static EventGroupHandle_t s_connected_event;
static bool               s_connected = false;
static bool               s_initialized = false;

/* MQTT check helpers */
#define WIFI_PROV_MQTT_BIT BIT0
static EventGroupHandle_t s_mqtt_event = NULL;

static void prov_mqtt_event(void *handler_args, esp_event_base_t base,
                            int32_t event_id, void *event_data)
{
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        xEventGroupSetBits(s_mqtt_event, WIFI_PROV_MQTT_BIT);
        break;
    case MQTT_EVENT_DISCONNECTED:
    case MQTT_EVENT_ERROR:
        xEventGroupClearBits(s_mqtt_event, WIFI_PROV_MQTT_BIT);
        break;
    default:
        break;
    }
}
/* Start a temporary MQTT client and wait for it to connect. The client
   is stopped and destroyed before returning. Returns ESP_OK on connect. */
esp_err_t wifi_prov_check_mqtt(const char *username, const char *password, TickType_t timeout_ticks)
{
    if (s_mqtt_event == NULL) {
        s_mqtt_event = xEventGroupCreate();
        if (s_mqtt_event == NULL) {
            ESP_LOGW(TAG, "Failed to create mqtt event group");
            return ESP_ERR_NO_MEM;
        }
    }

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_BROKER_URI,
        .broker.verification.certificate = (const char *)certificate_pem_start,
        .credentials = {
            .username = username,
            .authentication = {
                .password = password,
            },
        },
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "Failed to init mqtt client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, prov_mqtt_event, NULL);
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start mqtt client: %d", err);
        esp_mqtt_client_destroy(client);
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_mqtt_event, WIFI_PROV_MQTT_BIT, pdFALSE, pdTRUE, timeout_ticks);
    if ((bits & WIFI_PROV_MQTT_BIT) == 0) {
        ESP_LOGW(TAG, "MQTT did not connect within timeout");
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "MQTT connected during provisioning");
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    return ESP_OK;
}

/* Event base declared/defined in http_server.c */
ESP_EVENT_DECLARE_BASE(WIFI_PROV_EVENT);
enum { WIFI_PROV_EVENT_CREDENTIALS_SET };

typedef struct {
    char ssid[33];
    char password[65];
} wifi_prov_creds_t;

/* ── Portal credential callback ─────────────────────────────────────── */

static void on_credentials_set(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    /* No-op: teardown is performed by the caller that validated MQTT.
       Keep this handler to satisfy the event registration but avoid
       racing with the HTTP save flow. */
    (void)arg; (void)base; (void)id; (void)data;
}

esp_err_t wifi_prov_finalize_connection(void)
{
    ESP_LOGI(TAG, "Finalising provision: tearing down portal and switching to STA-only");
    http_server_stop();
    dns_server_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    s_connected = true;
    xEventGroupSetBits(s_connected_event, CONNECTED_BIT);
    if (s_config.on_connected) {
        s_config.on_connected();
    }
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t wifi_prov_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_prov_start(const wifi_prov_config_t *config)
{
    ESP_ERROR_CHECK(wifi_prov_init());

    s_config = *config;
    s_connected = false;
    s_connected_event = xEventGroupCreate();

    /* Register for portal credential events */
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_PROV_EVENT, WIFI_PROV_EVENT_CREDENTIALS_SET,
        on_credentials_set, NULL));

    /* Try loading stored credentials */
    char ssid[33]     = {0};
    char password[65] = {0};
    esp_err_t err = nvs_store_load(ssid, sizeof(ssid),
                                   password, sizeof(password));

    if (err == ESP_OK && ssid[0] != '\0') {
        ESP_LOGI(TAG, "Found stored credentials, attempting STA connection …");

        s_sta_netif = esp_netif_create_default_wifi_sta();
        wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));

        err = wifi_sta_connect(ssid, password, s_config.max_retries);
        if (err == ESP_OK) {
            /* Verify MQTT connectivity after successful STA connect from stored creds */
            char mqtt_token[65] = {0};
            nvs_store_load_mqtt_token(mqtt_token, sizeof(mqtt_token));
            /* Build MQTT username from MAC */
            char mqtt_user[32] = {0};
            uint8_t mac[6];
            if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
                snprintf(mqtt_user, sizeof(mqtt_user), "%02X%02X%02X%02X%02X%02X",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            }
            uint32_t wait_ms = (s_config.portal_timeout > 0) ? (s_config.portal_timeout * 1000) : 30000;
            esp_err_t mqtt_err = wifi_prov_check_mqtt(mqtt_user[0] ? mqtt_user : NULL,
                                                     mqtt_token[0] ? mqtt_token : NULL,
                                                     pdMS_TO_TICKS(wait_ms));
            if (mqtt_err == ESP_OK) {
                s_connected = true;
                xEventGroupSetBits(s_connected_event, CONNECTED_BIT);
                if (s_config.on_connected) {
                    s_config.on_connected();
                }
                return ESP_OK;
            }
            ESP_LOGW(TAG, "MQTT check failed for stored credentials (err=%d), starting provisioning portal", mqtt_err);
            /* Clean up STA state so we can start AP */
            esp_wifi_deinit();
            esp_netif_destroy_default_wifi(s_sta_netif);
            s_sta_netif = NULL;
        }

        ESP_LOGW(TAG, "STA connection failed, starting provisioning portal");
        /* wifi_sta_connect already called esp_wifi_stop() on failure,
           clean up the STA netif before starting AP */
        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    } else {
        ESP_LOGI(TAG, "No stored credentials, starting provisioning portal");
    }

    /* Start AP + captive portal */
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));

    wifi_ap_start(&s_config);
    dns_server_start();
    http_server_start(s_config.http_port, &s_config);

    if (s_config.on_portal_start) {
        s_config.on_portal_start();
    }

    return ESP_OK;
}

esp_err_t wifi_prov_stop(void)
{
    http_server_stop();
    dns_server_stop();
    wifi_ap_stop();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_connected_event) {
        vEventGroupDelete(s_connected_event);
        s_connected_event = NULL;
    }

    esp_event_handler_unregister(WIFI_PROV_EVENT,
                                 WIFI_PROV_EVENT_CREDENTIALS_SET,
                                 on_credentials_set);

    s_connected = false;
    return ESP_OK;
}

esp_err_t wifi_prov_wait_for_connection(TickType_t timeout_ticks)
{
    if (s_connected) {
        return ESP_OK;
    }

    EventBits_t bits = xEventGroupWaitBits(s_connected_event,
        CONNECTED_BIT, pdFALSE, pdTRUE, timeout_ticks);

    return (bits & CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_prov_erase_credentials(void)
{
    ESP_ERROR_CHECK(wifi_prov_init());
    return nvs_store_erase();
}

bool wifi_prov_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_prov_get_ip_info(esp_netif_ip_info_t *ip_info)
{
    if (!s_connected || !s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_get_ip_info(s_sta_netif, ip_info);
}
