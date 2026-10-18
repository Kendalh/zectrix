// Simulator stand-in for common/ai_chat_platform_esp.cc (esp_http_client + NVS).
//
// Real mode (default): sends the requests with the system `curl`, so the sim exercises the real
// request/response formats when the dev machine has network access. The key comes from DASHSCOPE_API_KEY
// or the /ai web page; they are kept in memory only.
// Fake mode (SIM_AI_FAKE=1): no network; a canned transcript (SIM_AI_SPEECH_TEXT overrides it) and a canned
// answer that reports how many context messages were sent, which lets tests check that a session remembers.
#pragma once

#include <memory>

#include "common/ai_chat.h"

std::unique_ptr<ai::ChatPlatform> NewSimChatPlatform();
