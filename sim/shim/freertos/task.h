#pragma once
/* Tasks become detached host threads. vTaskDelete(NULL) is a no-op: every task in the
 * firmware returns right after calling it, which ends the thread. */
#include <chrono>
#include <thread>
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack, void* arg,
                                     unsigned prio, TaskHandle_t* out) {
    (void)name; (void)stack; (void)prio;
    if (out) *out = nullptr;
    std::thread([fn, arg] { fn(arg); }).detach();
    return pdPASS;
}
static inline void vTaskDelete(TaskHandle_t) {}
static inline void vTaskDelay(TickType_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
