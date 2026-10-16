#pragma once
#include <stdint.h>
typedef struct { uint32_t addr; } esp_ip4_addr_t;
#define IP4_ADDR(p, a, b, c, d) ((p)->addr = ((uint32_t)(d) << 24) | ((uint32_t)(c) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(a))
