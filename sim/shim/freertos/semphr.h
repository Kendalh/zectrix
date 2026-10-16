#pragma once
#include "FreeRTOS.h"
typedef void* SemaphoreHandle_t;
#define xSemaphoreTake(m, t) ((void)(m), 1)
#define xSemaphoreGive(m) ((void)(m), 1)
