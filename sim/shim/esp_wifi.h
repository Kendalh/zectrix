#pragma once
/* Wi-Fi is not simulated: every call succeeds and the HTTP server binds to a local TCP port. */
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
typedef enum { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
typedef enum { WIFI_IF_STA, WIFI_IF_AP } wifi_interface_t;
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
#define WIFI_AUTH_WPA2_PSK 3
typedef struct {
    struct {
        uint8_t ssid[32]; uint8_t password[64]; uint8_t ssid_len; uint8_t channel;
        int authmode; uint8_t ssid_hidden; uint8_t max_connection; uint16_t beacon_interval;
    } ap;
} wifi_config_t;
static inline esp_err_t esp_wifi_scan_stop(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_stop(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_start(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_connect(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_set_mode(wifi_mode_t m) { (void)m; return ESP_OK; }
static inline esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t* c) { (void)i; (void)c; return ESP_OK; }
static inline esp_err_t esp_wifi_set_ps(wifi_ps_type_t t) { (void)t; return ESP_OK; }
