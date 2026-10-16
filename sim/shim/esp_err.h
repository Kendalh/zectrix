#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_WIFI_BASE 0x3000
#define ESP_ERR_WIFI_NOT_STARTED (ESP_ERR_WIFI_BASE + 3)
#define ESP_ERR_WIFI_CONN (ESP_ERR_WIFI_BASE + 6)
#define ESP_ERR_WIFI_STATE (ESP_ERR_WIFI_BASE + 7)
static inline const char* esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : e == ESP_FAIL ? "ESP_FAIL" : "ESP_ERR"; }
