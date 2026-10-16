#pragma once
#include <stdint.h>
#define portMAX_DELAY 0xffffffffu
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdPASS 1
#define pdFAIL 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
