/**
 * @file token_board_platform_esp.h
 * @brief Device implementation of board::Platform (HTTPS via the AI chat platform + NVS). Firmware only.
 */

#ifndef TOKEN_BOARD_PLATFORM_ESP_H
#define TOKEN_BOARD_PLATFORM_ESP_H

#include <memory>

#include "token_board.h"

std::unique_ptr<board::Platform> NewEspBoardPlatform();

#endif  // TOKEN_BOARD_PLATFORM_ESP_H
