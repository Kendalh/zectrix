/**
 * @file token_board_platform_esp.cc
 * @brief board::Platform for the device. HTTPS goes through the AI chat platform (one short task per request,
 *        TLS with the certificate bundle); the AccessKey and the balance baselines live in NVS namespace "tokenboard".
 */

#include "token_board_platform_esp.h"

#include "ai_chat_platform_esp.h"
#include "settings.h"

namespace {

constexpr char kNamespace[] = "tokenboard";

class EspBoardPlatform : public board::Platform {
public:
    EspBoardPlatform() : http_(NewEspChatPlatform()) {}

    void Send(ai::ChatPlatform::HttpRequest request, ai::ChatPlatform::DoneCallback done) override {
        http_->Send(std::move(request), std::move(done));
    }
    std::string Load(const std::string& key) override { return Settings(kNamespace, false).GetString(key, ""); }
    void Save(const std::string& key, const std::string& value) override {
        Settings(kNamespace, true).SetString(key, value);
    }

private:
    std::unique_ptr<ai::ChatPlatform> http_;
};

}  // namespace

std::unique_ptr<board::Platform> NewEspBoardPlatform() { return std::make_unique<EspBoardPlatform>(); }
