#pragma once
#include <stdio.h>
#ifndef SIM_LOG_VERBOSE
#define SIM_LOG_VERBOSE 0
#endif
#define ESP_LOGI(tag, fmt, ...) do { if (SIM_LOG_VERBOSE) fprintf(stderr, "I (%s) " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { } while (0)
