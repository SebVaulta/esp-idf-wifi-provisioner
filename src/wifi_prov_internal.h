/*
 * SPDX-FileCopyrightText: 2026 Michael Teeuw
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Internal declarations shared between component source files.
 * Not part of the public API.
 */

#pragma once

#include "wifi_provisioner.h"
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "esp_netif.h"
#include "esp_log.h"

#include <string.h>

/* ── NVS store ──────────────────────────────────────────────────────── */

esp_err_t nvs_store_load(char *ssid, size_t ssid_len,
                         char *password, size_t pass_len);
esp_err_t nvs_store_save(const char *ssid, const char *password);
esp_err_t nvs_store_save_mqtt_token(const char *mqtt_token);
esp_err_t nvs_store_load_mqtt_token(char *mqtt_token, size_t len);
esp_err_t nvs_store_erase(void);

/* ── WiFi STA ───────────────────────────────────────────────────────── */

esp_err_t wifi_sta_connect(const char *ssid, const char *password,
                           uint8_t max_retries);
esp_err_t wifi_sta_try_connect(const char *ssid, const char *password);

/* ── WiFi AP ────────────────────────────────────────────────────────── */

esp_err_t wifi_ap_start(const wifi_prov_config_t *config);
esp_err_t wifi_ap_stop(void);

/* ── DNS server ─────────────────────────────────────────────────────── */

esp_err_t dns_server_start(void);
esp_err_t dns_server_stop(void);

/* ── HTTP server ────────────────────────────────────────────────────── */

esp_err_t http_server_start(uint16_t port, const wifi_prov_config_t *config);
esp_err_t http_server_stop(void);
esp_err_t dns_server_stop(void);

/* Finalise a successful provision: tear down portal/AP and mark STA connected. */
esp_err_t wifi_prov_finalize_connection(void);

/* Check MQTT connectivity using provided username/password. Blocks for
    up to timeout_ticks ticks. Returns ESP_OK on successful CONNECT. */
esp_err_t wifi_prov_check_mqtt(const char *username, const char *password, TickType_t timeout_ticks);
