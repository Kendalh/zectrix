/**
 * @file ai_chat_platform_esp.h
 * @brief Device implementation of ai::ChatPlatform (HTTPS + NVS). Firmware only.
 */

#ifndef AI_CHAT_PLATFORM_ESP_H
#define AI_CHAT_PLATFORM_ESP_H

#include <memory>

#include "ai_chat.h"

std::unique_ptr<ai::ChatPlatform> NewEspChatPlatform();

#endif  // AI_CHAT_PLATFORM_ESP_H
