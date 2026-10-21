#include "fake/ai_chat_platform_fake.h"

#include <unistd.h>

#include <cJSON.h>

#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <mutex>
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

// What the "speaker" said: the first line of the file SIM_AI_SPEECH_FILE (read on every recording, so it can be edited
// between key presses), else SIM_AI_SPEECH_TEXT, else a canned sentence.
std::string SpokenText() {
    if (const char* path = getenv("SIM_AI_SPEECH_FILE")) {
        std::ifstream f(path);
        std::string line;
        if (std::getline(f, line) && !line.empty()) return line;
    }
    const char* spoken = getenv("SIM_AI_SPEECH_TEXT");
    return spoken ? spoken : "（模拟语音）今天天气怎么样";
}

bool SpeechOverridden() { return getenv("SIM_AI_SPEECH_FILE") || getenv("SIM_AI_SPEECH_TEXT"); }

std::string FakeTranscriptReply() {
    const std::string spoken = SpokenText();
    cJSON* reply = cJSON_CreateObject();
    cJSON* choices = cJSON_AddArrayToObject(reply, "choices");
    cJSON* choice = cJSON_CreateObject();
    cJSON* message = cJSON_AddObjectToObject(choice, "message");
    cJSON_AddStringToObject(message, "content", spoken.c_str());
    cJSON_AddItemToArray(choices, choice);
    char* out = cJSON_PrintUnformatted(reply);
    std::string json = out;
    cJSON_free(out);
    cJSON_Delete(reply);
    return json;
}

// ---- AI painter fakes: a canned image reply and a synthetic 800x600 PNG (stored deflate blocks, no compression) ----

uint32_t Crc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

void Put32(std::string* s, uint32_t v) {
    for (int i = 3; i >= 0; --i) s->push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

void PutChunk(std::string* png, const char* type, const std::string& body) {
    Put32(png, static_cast<uint32_t>(body.size()));
    const std::string typed = std::string(type, 4) + body;
    png->append(typed);
    Put32(png, Crc32(typed));
}

// A sunset poster: paper frame like the model's, yellow sky, red sun, black mountain and water band.
std::array<int, 3> PosterPixel(int x, int y) {
    if (x < 24 || y < 18 || x >= 776 || y >= 582) return {250, 245, 230};
    const int dx = x - 560, dy = y - 190;
    if (dx * dx + dy * dy < 90 * 90) return {255, 0, 0};
    if (y > 480) return (y / 6) % 2 ? std::array<int, 3>{0, 0, 0} : std::array<int, 3>{255, 255, 255};
    const int peak = 470 - std::max(0, 380 - std::abs(x - 300)) * 5 / 4;  // triangle
    if (y > peak) return {0, 0, 0};
    const int t = std::min(255, y * 255 / 480);
    return {255, 255, std::max(0, 255 - t)};
}

std::string FakePosterPng() {
    constexpr int kW = 800, kH = 600;
    std::string raw;
    raw.reserve((kW * 3 + 1) * kH);
    for (int y = 0; y < kH; ++y) {
        raw.push_back(0);
        for (int x = 0; x < kW; ++x) {
            const auto c = PosterPixel(x, y);
            raw.push_back(static_cast<char>(c[0]));
            raw.push_back(static_cast<char>(c[1]));
            raw.push_back(static_cast<char>(c[2]));
        }
    }
    std::string z = "\x78\x01";
    for (size_t pos = 0; pos < raw.size(); pos += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<char>(n & 0xFF));
        z.push_back(static_cast<char>(n >> 8));
        z.push_back(static_cast<char>(~n & 0xFF));
        z.push_back(static_cast<char>((~n >> 8) & 0xFF));
        z.append(raw, pos, n);
    }
    uint32_t a = 1, b = 0;
    for (unsigned char ch : raw) {
        a = (a + ch) % 65521;
        b = (b + a) % 65521;
    }
    Put32(&z, (b << 16) | a);
    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    Put32(&ihdr, kW);
    Put32(&ihdr, kH);
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);
    PutChunk(&png, "IHDR", ihdr);
    PutChunk(&png, "IDAT", z);
    PutChunk(&png, "IEND", "");
    return png;
}

// The image the fake "downloads": SIM_AI_IMAGE_PNG (a PNG file) if set, else the synthetic poster.
std::string FakeImageBytes() {
    if (const char* path = getenv("SIM_AI_IMAGE_PNG")) {
        std::ifstream f(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return FakePosterPng();
}

constexpr char kFakeImageUrl[] = "fake://image.png";

class SimChatPlatform : public ai::ChatPlatform {
public:
    void Send(HttpRequest request, DoneCallback done) override {
        std::thread([request = std::move(request), done = std::move(done)]() mutable {
            // Real network, but the sim records only silence: with SIM_AI_SPEECH_FILE/_TEXT set the recognition step
            // is answered locally and everything else (chat, image model, download) is the real service.
            if (!FakeMode() && !request.body_file.empty() && SpeechOverridden()) {
                if (!ai::BuildFileBody(&request)) done(0, "", "录音文件读取失败");
                else done(200, FakeTranscriptReply(), "");
                return;
            }
            if (!FakeMode()) {
                RealRequest(std::move(request), done);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!request.body_file.empty()) {  // speech recognition: like the device, read the recording then delete it
                if (!ai::BuildFileBody(&request)) done(0, "", "录音文件读取失败");
                else done(200, FakeTranscriptReply(), "");
            } else if (request.url.find("multimodal-generation") != std::string::npos) {  // AI painter: image model
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                done(200, std::string("{\"output\":{\"choices\":[{\"message\":{\"content\":[{\"image\":\"") + kFakeImageUrl +
                             "\"}]}}]}}", "");
            } else if (request.method == "GET" && request.url == kFakeImageUrl) {  // AI painter: download
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                done(200, FakeImageBytes(), "");
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                done(200, CannedReply(request.body), "");
            }
        }).detach();
    }

    // The device keeps the key in NVS, which AI Q&A and AI painter (each has its own platform instance) both read. The
    // sim shares one in-memory slot between instances for the same reason. A key saved on the /ai web page wins over
    // DASHSCOPE_API_KEY; nothing is written to disk, so it is gone when the simulator restarts.
    std::string LoadApiKey() override {
        {
            std::lock_guard<std::mutex> lock(SavedKeyMutex());
            if (!SavedKey().empty()) return SavedKey();
        }
        const char* env = getenv("DASHSCOPE_API_KEY");
        return env ? env : (FakeMode() ? "sim-fake-key" : "");
    }
    void SaveApiKey(const std::string& key) override {
        std::lock_guard<std::mutex> lock(SavedKeyMutex());
        SavedKey() = key;
    }

private:
    static std::string& SavedKey() {
        static std::string key;
        return key;
    }
    static std::mutex& SavedKeyMutex() {
        static std::mutex mu;
        return mu;
    }
};

}  // namespace

std::unique_ptr<ai::ChatPlatform> NewSimChatPlatform() { return std::make_unique<SimChatPlatform>(); }
