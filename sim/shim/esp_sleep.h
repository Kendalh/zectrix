#pragma once
#include <stdint.h>
#include <stdio.h>
#include "driver/gpio.h"
static inline int esp_sleep_enable_ext0_wakeup(gpio_num_t g, int level) { (void)g; (void)level; return 0; }
/* Real device powers off here; the simulator only logs it so tests can keep running. */
static inline void esp_deep_sleep_start(void) { fprintf(stderr, "[sim] esp_deep_sleep_start() called (ignored)\n"); }
