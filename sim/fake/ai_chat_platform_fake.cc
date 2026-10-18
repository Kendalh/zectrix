#include "fake/ai_chat_platform_fake.h"

#include <unistd.h>

#include <cJSON.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

bool FakeMode() {
    const char* v = getenv("SIM_AI_FAKE");
    return v && *v && *v != '0';
}

std::string CannedReply(const std::string& request) {
    int turns = 0;
    std::string last_question;
    if (cJSON* root = cJSON_Parse(request.c_str())) {
        cJSON* msgs = cJSON_GetObjectItemCaseSensitive(root, "messages");
        cJSON* m = nullptr;
        cJSON_ArrayForEach(m, msgs) {
            cJSON* role = cJSON_GetObjectItemCaseSensitive(m, "role");
            cJSON* content = cJSON_GetObjectItemCaseSensitive(m, "content");
            if (!cJSON_IsString(role) || !cJSON_IsString(content)) continue;
            if (std::string(role->valuestring) == "system") continue;
            ++turns;
            if (std::string(role->valuestring) == "user") last_question = content->valuestring;
        }
        cJSON_Delete(root);
    }
    cJSON* reply = cJSON_CreateObject();
    cJSON* choices = cJSON_AddArrayToObject(reply, "choices");
    cJSON* choice = cJSON_CreateObject();
    cJSON* message = cJSON_AddObjectToObject(choice, "message");
    cJSON_AddStringToObject(message, "role", "assistant");
    const std::string text = "（模拟回答）你问的是：" + last_question + "\n本次共发送 " + std::to_string(turns) +
                             " 条上下文消息。";
    cJSON_AddStringToObject(message, "content", text.c_str());
    cJSON_AddItemToArray(choices, choice);
    char* out = cJSON_PrintUnformatted(reply);
    std::string s = out;
    cJSON_free(out);
    cJSON_Delete(reply);
    return s;
}

// curl reads URL, headers (with the key) and body from files, so secrets never show up in `ps`.
void RealRequest(ai::ChatPlatform::HttpRequest req, const ai::ChatPlatform::DoneCallback& done) {
    if (!req.body_file.empty() && !ai::BuildFileBody(&req)) {
        done(0, "", "录音文件读取失败");
        return;
    }
    char body_path[] = "/tmp/sim_ai_body_XXXXXX";
    char cfg_path[] = "/tmp/sim_ai_cfg_XXXXXX";
    int body_fd = mkstemp(body_path);
    int cfg_fd = mkstemp(cfg_path);
    if (body_fd < 0 || cfg_fd < 0) {
        done(0, "", "cannot create temp files");
        return;
    }
    auto write_all = [](int fd, const std::string& s) {
        (void)!write(fd, s.data(), s.size());
        close(fd);
    };
    write_all(body_fd, req.body);
    std::string cfg = "url = \"" + req.url + "\"\n";
    for (const auto& h : req.headers) cfg += "header = \"" + h.first + ": " + h.second + "\"\n";
    if (req.method == "GET") cfg += "request = \"GET\"\n";
    else cfg += std::string("data-binary = \"@") + body_path + "\"\n";
    write_all(cfg_fd, cfg);

    const std::string cmd = std::string("curl -q -sS -m 90 -K ") + cfg_path + " -w '\\n%{http_code}' 2>&1";
    std::string output;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0) output.append(buf, n);
        pclose(p);
    }
    unlink(body_path);
    unlink(cfg_path);

    const size_t nl = output.rfind('\n');
    const int status = nl == std::string::npos ? 0 : atoi(output.c_str() + nl + 1);
    if (status == 0) {
        done(0, "", output.empty() ? "curl failed" : output.substr(0, 120));
    } else {
        done(status, output.substr(0, nl), "");
    }
}

std::string FakeTranscriptReply() {
    const char* spoken = getenv("SIM_AI_SPEECH_TEXT");  // what the "speaker" said (tests)
    cJSON* reply = cJSON_CreateObject();
    cJSON* choices = cJSON_AddArrayToObject(reply, "choices");
    cJSON* choice = cJSON_CreateObject();
    cJSON* message = cJSON_AddObjectToObject(choice, "message");
    cJSON_AddStringToObject(message, "content", spoken ? spoken : "（模拟语音）今天天气怎么样");
    cJSON_AddItemToArray(choices, choice);
    char* out = cJSON_PrintUnformatted(reply);
    std::string json = out;
    cJSON_free(out);
    cJSON_Delete(reply);
    return json;
}

class SimChatPlatform : public ai::ChatPlatform {
public:
    void Send(HttpRequest request, DoneCallback done) override {
        std::thread([request = std::move(request), done = std::move(done)]() mutable {
            if (!FakeMode()) {
                RealRequest(std::move(request), done);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!request.body_file.empty()) {  // speech recognition: like the device, read the recording then delete it
                if (!ai::BuildFileBody(&request)) done(0, "", "录音文件读取失败");
                else done(200, FakeTranscriptReply(), "");
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                done(200, CannedReply(request.body), "");
            }
        }).detach();
    }

    std::string LoadApiKey() override {
        const char* env = getenv("DASHSCOPE_API_KEY");
        return env ? env : (FakeMode() ? "sim-fake-key" : "");
    }
    void SaveApiKey(const std::string&) override {}  // in memory only (held by ChatService)
};

}  // namespace

std::unique_ptr<ai::ChatPlatform> NewSimChatPlatform() { return std::make_unique<SimChatPlatform>(); }
