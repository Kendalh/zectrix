#pragma once
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Simulated clock: tests advance it explicitly. */
int64_t esp_timer_get_time(void);

/* Timers never fire in the simulator (it has no event loop); the calls only have to compile. */
typedef struct sim_esp_timer* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void* arg);
typedef enum { ESP_TIMER_TASK, ESP_TIMER_ISR } esp_timer_dispatch_t;
typedef struct {
    esp_timer_cb_t callback;
    void* arg;
    esp_timer_dispatch_t dispatch_method;
    const char* name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;
static inline esp_err_t esp_timer_create(const esp_timer_create_args_t* a, esp_timer_handle_t* out) { (void)a; *out = (esp_timer_handle_t)1; return ESP_OK; }
static inline esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { (void)t; (void)us; return ESP_OK; }
static inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us) { (void)t; (void)us; return ESP_OK; }
static inline esp_err_t esp_timer_stop(esp_timer_handle_t t) { (void)t; return ESP_OK; }
static inline esp_err_t esp_timer_delete(esp_timer_handle_t t) { (void)t; return ESP_OK; }
void sim_advance_time_us(int64_t us);
#ifdef __cplusplus
}
#endif
