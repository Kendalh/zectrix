#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Simulated clock: tests advance it explicitly. */
int64_t esp_timer_get_time(void);
void sim_advance_time_us(int64_t us);
#ifdef __cplusplus
}
#endif
