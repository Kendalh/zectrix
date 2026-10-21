// Simulator stand-in for common/token_board_platform_esp.cc (esp_http_client + NVS).
//
// Real mode (default): the requests go through the AI chat sim platform (system `curl`), so the sim exercises the real
// Alibaba Cloud BSS endpoint. The AccessKey comes from ALIBABA_CLOUD_ACCESS_KEY_ID / _SECRET or the /tokenboard web
// page; it and the balance baselines are kept in memory only. DeepSeek: DEEPSEEK_API_KEY or the same page.
// Fake mode (SIM_BOARD_FAKE=1): no network; canned balance (it drops 0.05 per refresh, so the today/hour estimates
// move) and canned yesterday/today bills in the real response shape.
#pragma once

#include <memory>

#include "common/token_board.h"

std::unique_ptr<board::Platform> NewSimBoardPlatform();
