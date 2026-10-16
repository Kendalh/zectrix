#pragma once
#include "esp_err.h"
#include "lwip/ip_addr.h"
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct sim_netif esp_netif_t;
static inline esp_netif_t* esp_netif_create_default_wifi_ap(void) { static int x; return (esp_netif_t*)&x; }
static inline void esp_netif_destroy_default_wifi(esp_netif_t* n) { (void)n; }
static inline esp_err_t esp_netif_dhcps_stop(esp_netif_t* n) { (void)n; return ESP_OK; }
static inline esp_err_t esp_netif_dhcps_start(esp_netif_t* n) { (void)n; return ESP_OK; }
static inline esp_err_t esp_netif_set_ip_info(esp_netif_t* n, const esp_netif_ip_info_t* i) { (void)n; (void)i; return ESP_OK; }
