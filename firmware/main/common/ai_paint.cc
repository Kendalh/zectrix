#include "ai_paint.h"

#include <cJSON.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <thread>

#include "photo_storage.h"
#include "png_decode.h"

namespace ai {
namespace paint {

namespace {

// The look of the Pudong and Fuji posters. The scene itself (what is drawn, time of day, sun or moon) comes from the planner;
// only the style that must hold for every picture is fixed here.
const char kPlannerSystem[] =
    "You write image prompts for a flat-vector screen-print poster that will be shown on a four-color e-ink display. "
    "The only inks are pure black, pure white, pure yellow and pure red.\n"
    "The user gives a short subject (often Chinese, spoken aloud). Reply with ONE English paragraph of at most 90 words "
    "describing the poster scene, and nothing else: no preface, no quotes, no lists, no Markdown.\n"
    "Rules:\n"
    "1. Name the subject's real, recognisable features (for a landmark: its true shape, towers, arches, structure; for an "
    "animal or person: pose and silhouette). It is the large, centered hero, drawn as a solid pure-black silhouette with thin "
    "white detail lines.\n"
    "2. Take the time of day, weather, season and mood from the user's words; if none is given, use sunset. Then choose the sky "
    "and the one big celestial disc to match: sunset or sunrise = a giant red sun disc behind the subject, sky fading from "
    "white to bright yellow to orange with halftone dots, thin white cloud streaks; daytime = a bright yellow sun, white and "
    "yellow sky; night = a large white or pale-yellow full moon in a solid black sky with a few white stars and a few yellow "
    "lit windows, never a sun; rain, snow, fog or heavy cloud = an overcast sky of white and yellow halftone dots with thin "
    "falling streaks and NO sun, NO moon and NO circular disc anywhere in the picture. Never draw a sun at night and never "
    "draw a moon in daytime.\n"
    "3. Add a fitting foreground: water with broken horizontal black reflection lines and small red-yellow glints, or ground, "
    "grass or street; plus a few small details that suit the scene (a few black birds by day, trees, boats).\n"
    "4. Use only black, white, yellow and red, and say which element has which color. No gradients except halftone dots. No "
    "text, no letters, no logos, no frame or border.";
const char kPosterPrefix[] = "Flat vector screen-print poster. ";
const char kPosterSuffix[] =
    " Solid pure-black silhouettes with thin white detail lines, halftone dots for sky gradients, broken horizontal black "
    "reflection lines on water. STRICT four-color palette only: pure black, pure white, pure yellow, pure red. Bold hard "
    "edges, retro, high contrast, e-ink friendly.";
const char kFallbackPrefix[] = "Flat vector screen-print poster of ";
const char kFallbackMiddle[] =
    ". The subject is large, centered and simplified into bold shapes, drawn as a solid pure-black silhouette with thin white "
    "detail lines. One huge celestial disc behind it that matches the time of day in the subject: a giant red sun at "
    "sunset (the default), a bright yellow sun by day, a large white moon at night with a black sky and a few white stars. "
    "The sky fades from white to bright yellow to orange with halftone dots, except at night. Foreground water or ground "
    "with broken horizontal black reflection lines.";
const char kNegativePrompt[] =
    "photograph, realistic, 3D render, blue, green, purple, gray, soft shadow, blurry, text, letters, watermark, many colors";
constexpr size_t kMaxPlanChars = 1000;

bool IsUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

size_t Utf8Len(unsigned char lead) {
    if (lead >= 0xF0) return 4;
    if (lead >= 0xE0) return 3;
    if (lead >= 0xC0) return 2;
    return 1;
}

bool IsFiller(const std::string& ch) {
    static const char* const kFiller[] = {"。", "，", "！", "？", "、", "…", "；", "：", "“", "”", "～"};
    if (ch.size() == 1) return !isalnum(static_cast<unsigned char>(ch[0])) && ch[0] != '(' && ch[0] != ')';
    for (const char* f : kFiller)
        if (ch == f) return true;
    return false;
}

std::vector<std::string> SplitChars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const size_t n = std::min(Utf8Len(static_cast<unsigned char>(s[i])), s.size() - i);
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

// Palette in the panel's 2-bit order.
struct Ink {
    int r, g, b;
    uint8_t code;
};
constexpr Ink kInks[4] = {{0, 0, 0, 0}, {255, 255, 255, 1}, {255, 255, 0, 2}, {255, 0, 0, 3}};

int Clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

}  // namespace

std::string NormalizeSubject(const std::string& raw) {
    std::vector<std::string> chars = SplitChars(raw);
    size_t b = 0, e = chars.size();
    while (b < e && (IsFiller(chars[b]) || isspace(static_cast<unsigned char>(chars[b][0])))) ++b;
    while (e > b && (IsFiller(chars[e - 1]) || isspace(static_cast<unsigned char>(chars[e - 1][0])))) --e;
    std::string out;
    size_t count = 0;
    for (size_t i = b; i < e && count < kMaxSubjectChars; ++i, ++count) {
        out += (chars[i] == "\n" || chars[i] == "\r") ? " " : chars[i];
    }
    return out;
}

const char* PlannerSystemPrompt() { return kPlannerSystem; }

std::string BuildPlanRequestJson(const ChatConfig& config, const std::string& subject) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", config.model.c_str());
    cJSON* messages = cJSON_AddArrayToObject(root, "messages");
    cJSON* system = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content", kPlannerSystem);
    cJSON_AddItemToArray(messages, system);
    cJSON* user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", subject.c_str());
    cJSON_AddItemToArray(messages, user);
    cJSON_AddNumberToObject(root, "max_tokens", 300);
    cJSON_AddBoolToObject(root, "stream", false);
    char* printed = cJSON_PrintUnformatted(root);
    std::string json = printed ? printed : "";
    cJSON_free(printed);
    cJSON_Delete(root);
    return json;
}

bool CleanPlan(const std::string& raw, std::string* description) {
    std::string text;
    for (char ch : raw) {  // one line; drop Markdown marks and quotes the model may wrap around the paragraph
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        if (ch == '*' || ch == '#' || ch == '`' || ch == '"') continue;
        if (ch == ' ' && !text.empty() && text.back() == ' ') continue;
        text += ch;
    }
    size_t b = 0, e = text.size();
    while (b < e && text[b] == ' ') ++b;
    while (e > b && text[e - 1] == ' ') --e;
    text = text.substr(b, e - b);
    if (text.size() > kMaxPlanChars) {
        size_t cut = kMaxPlanChars;
        while (cut > 0 && IsUtf8Continuation(static_cast<unsigned char>(text[cut]))) --cut;
        text.resize(cut);
    }
    if (text.size() < 20) return false;  // an empty or one-word answer is not a scene
    *description = text;
    return true;
}

std::string ImagePrompt(const std::string& description) {
    return std::string(kPosterPrefix) + description + kPosterSuffix;
}

std::string FallbackPrompt(const std::string& subject) {
    return std::string(kFallbackPrefix) + subject + kFallbackMiddle + kPosterSuffix;
}

const char* NegativePrompt() { return kNegativePrompt; }

std::string BuildImageRequestJson(const PaintConfig& config, const std::string& prompt) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", config.model.c_str());
    cJSON* input = cJSON_AddObjectToObject(root, "input");
    cJSON* messages = cJSON_AddArrayToObject(input, "messages");
    cJSON* message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "user");
    cJSON* content = cJSON_AddArrayToObject(message, "content");
    cJSON* part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", prompt.c_str());
    cJSON_AddItemToArray(content, part);
    cJSON_AddItemToArray(messages, message);
    cJSON* params = cJSON_AddObjectToObject(root, "parameters");
    cJSON_AddStringToObject(params, "negative_prompt", kNegativePrompt);
    cJSON_AddStringToObject(params, "size", config.size.c_str());
    cJSON_AddNumberToObject(params, "n", 1);
    cJSON_AddBoolToObject(params, "prompt_extend", false);  // keep our wording
    cJSON_AddBoolToObject(params, "watermark", false);
    char* printed = cJSON_PrintUnformatted(root);
    std::string json = printed ? printed : "";
    cJSON_free(printed);
    cJSON_Delete(root);
    return json;
}

std::string FriendlyError(const std::string& e) {
    auto has = [&](const char* needle) { return e.find(needle) != std::string::npos; };
    if (has("rate limit") || has("Throttling") || has("Too many requests")) return "请求太频繁，请稍后再试";
    if (has("Arrearage") || has("overdue") || has("quota") || has("insufficient")) return "额度不足或账号欠费";
    if (has("Invalid API-key") || has("InvalidApiKey")) return "API Key 无效";
    if (has("DataInspectionFailed") || has("inappropriate content")) return "内容未通过审核，换个说法试试";
    return e;
}

bool ParseImageResponse(const std::string& body, std::string* image_url, std::string* error) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        *error = "响应格式错误";
        return false;
    }
    bool ok = false;
    cJSON* output = cJSON_GetObjectItemCaseSensitive(root, "output");
    cJSON* choices = cJSON_GetObjectItemCaseSensitive(output, "choices");
    cJSON* first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
    cJSON* parts = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(first, "message"), "content");
    cJSON* part = cJSON_IsArray(parts) ? cJSON_GetArrayItem(parts, 0) : nullptr;
    cJSON* image = cJSON_GetObjectItemCaseSensitive(part, "image");
    cJSON* message = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (cJSON_IsString(image) && image->valuestring && image->valuestring[0]) {
        *image_url = image->valuestring;
        ok = true;
    } else if (cJSON_IsString(message) && message->valuestring) {
        *error = message->valuestring;
    } else {
        *error = "没有收到图片";
    }
    cJSON_Delete(root);
    return ok;
}

bool PngToBwry(const std::string& png_bytes, std::vector<uint8_t>* out, std::string* error) {
    png::Image img;
    std::string why;
    if (!png::Decode(reinterpret_cast<const uint8_t*>(png_bytes.data()), png_bytes.size(), &img, &why)) {
        *error = "图片解码失败: " + why;
        return false;
    }
    // The model paints a paper-coloured frame around the poster: crop 3% on every side, then area-average to the panel size.
    const int cx = static_cast<int>(img.width * 3 / 100), cy = static_cast<int>(img.height * 3 / 100);
    const int cw = static_cast<int>(img.width) - 2 * cx, ch = static_cast<int>(img.height) - 2 * cy;
    if (cw < kWidth / 2 || ch < kHeight / 2) {
        *error = "图片尺寸太小";
        return false;
    }
    std::vector<int16_t> row_a((kWidth + 2) * 3, 0), row_b((kWidth + 2) * 3, 0);  // Floyd-Steinberg error rows
    int16_t* err_cur = row_a.data() + 3;
    int16_t* err_next = row_b.data() + 3;
    out->assign(kImageBytes, 0);
    for (int y = 0; y < kHeight; ++y) {
        const int sy0 = cy + y * ch / kHeight;
        const int sy1 = std::max(sy0 + 1, cy + (y + 1) * ch / kHeight);
        std::fill(err_next - 3, err_next - 3 + (kWidth + 2) * 3, 0);  // the row about to receive error
        for (int x = 0; x < kWidth; ++x) {
            const int sx0 = cx + x * cw / kWidth;
            const int sx1 = std::max(sx0 + 1, cx + (x + 1) * cw / kWidth);
            int sum[3] = {0, 0, 0}, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint8_t* p = img.rgb.data() + (static_cast<size_t>(sy) * img.width + sx0) * 3;
                for (int sx = sx0; sx < sx1; ++sx, p += 3, ++n) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            int c[3] = {sum[0] / n, sum[1] / n, sum[2] / n};
            const int gray = (c[0] * 299 + c[1] * 587 + c[2] * 114) / 1000;
            for (int k = 0; k < 3; ++k) {  // saturation x1.3, then contrast x1.15 around mid grey: poster inks, not photo tones
                int v = gray + (c[k] - gray) * 13 / 10;
                v = 128 + (v - 128) * 115 / 100;
                c[k] = Clamp255(v) + err_cur[x * 3 + k];
            }
            int best = 0;
            long best_d = -1;
            for (int i = 0; i < 4; ++i) {
                const long dr = c[0] - kInks[i].r, dg = c[1] - kInks[i].g, db = c[2] - kInks[i].b;
                const long d = dr * dr + dg * dg + db * db;
                if (best_d < 0 || d < best_d) {
                    best_d = d;
                    best = i;
                }
            }
            const int e[3] = {c[0] - kInks[best].r, c[1] - kInks[best].g, c[2] - kInks[best].b};
            for (int k = 0; k < 3; ++k) {
                err_cur[(x + 1) * 3 + k] += static_cast<int16_t>(e[k] * 7 / 16);
                err_next[(x - 1) * 3 + k] += static_cast<int16_t>(e[k] * 3 / 16);
                err_next[x * 3 + k] += static_cast<int16_t>(e[k] * 5 / 16);
                err_next[(x + 1) * 3 + k] += static_cast<int16_t>(e[k] * 1 / 16);
            }
            const size_t pixel = static_cast<size_t>(y) * kWidth + x;
            (*out)[pixel >> 2] |= static_cast<uint8_t>(kInks[best].code << (6 - (pixel & 3) * 2));
        }
        std::swap(err_cur, err_next);
    }
    return true;
}

// ---- service ----------------------------------------------------------------------------------

PaintService& PaintService::Get() {
    static PaintService instance;
    return instance;
}

void PaintService::Start(std::unique_ptr<ChatPlatform> platform) {
    std::lock_guard<std::mutex> lock(mu_);
    if (platform_) return;
    platform_ = std::move(platform);
    api_key_ = platform_->LoadApiKey();
}

bool PaintService::Started() {
    std::lock_guard<std::mutex> lock(mu_);
    return platform_ != nullptr;
}

void PaintService::SetOnChange(std::function<void()> on_change) {
    std::lock_guard<std::mutex> lock(mu_);
    on_change_ = std::move(on_change);
}

bool PaintService::HasApiKey() {
    std::lock_guard<std::mutex> lock(mu_);
    if (platform_) api_key_ = platform_->LoadApiKey();  // re-read: the key can be changed on the web page at any time
    return !api_key_.empty();
}

void PaintService::Notify() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = on_change_;
    }
    if (cb) cb();
}

bool PaintService::BeginLocked(Phase phase, std::string* error) {
    if (!platform_) {
        *error = "AI 服务未启动，请先连接 Wi-Fi";
        return false;
    }
    api_key_ = platform_->LoadApiKey();
    if (api_key_.empty()) {
        *error = "请先设置百炼 API Key";
        return false;
    }
    if (busy_) {
        *error = "正在绘制，请稍候";
        return false;
    }
    busy_ = true;
    status_.phase = phase;
    status_.working = true;
    status_.error.clear();
    return true;
}

bool PaintService::VoiceReady(std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!platform_) {
        *error = "AI 服务未启动，请先连接 Wi-Fi";
        return false;
    }
    api_key_ = platform_->LoadApiKey();
    if (api_key_.empty()) {
        *error = "请先设置百炼 API Key";
        return false;
    }
    if (busy_) {
        *error = "正在绘制，请稍候";
        return false;
    }
    return true;
}

void PaintService::SetError(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (busy_) return;
        status_.phase = Phase::Error;
        status_.error = message;
    }
    Notify();
}

void PaintService::SetPhase(Phase phase) {
    std::lock_guard<std::mutex> lock(mu_);
    status_.phase = phase;
}

void PaintService::Fail(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        busy_ = false;
        status_.working = false;
        status_.phase = Phase::Error;
        status_.error = error;
    }
    Notify();
}

bool PaintService::AskVoice(const std::string& wav_path, std::string* error) {
    ChatPlatform* platform;
    ChatConfig config;
    std::string key;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!BeginLocked(Phase::Transcribing, error)) return false;
        platform = platform_.get();
        config = chat_config_;
        key = api_key_;
    }
    Notify();
    ChatPlatform::HttpRequest request;
    request.url = config.url;
    request.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + key}};
    request.body_file = wav_path;  // the platform reads and deletes it on its own task
    SpeechBodyParts(config, &request.body_prefix, &request.body_suffix);
    platform->Send(std::move(request), [this](int status, std::string body, std::string transport_error) {
        OnTranscribed(status, std::move(body), std::move(transport_error));
    });
    return true;
}

void PaintService::OnTranscribed(int status, std::string body, std::string transport_error) {
    if (!transport_error.empty()) return Fail("语音识别失败: 网络错误 " + transport_error);
    std::string text, error;
    if (!ParseResponse(body, &text, &error)) {
        if (status == 401) error = "API Key 无效 (401)";
        else if (status != 200 && (error == "没有收到回答" || error == "响应格式错误"))
            error = "请求失败 (HTTP " + std::to_string(status) + ")";
        return Fail("语音识别失败: " + error);
    }
    const std::string subject = NormalizeSubject(text);
    if (!IsUsableTranscript(subject)) return Fail("没听清，请再说一次");
    StartPlan(subject);
}

bool PaintService::Draw(const std::string& raw_subject, std::string* error) {
    const std::string subject = NormalizeSubject(raw_subject);
    if (subject.empty()) {
        *error = "没有要画的内容";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!BeginLocked(Phase::Planning, error)) return false;
    }
    Notify();
    StartPlan(subject);
    return true;
}

bool PaintService::Regenerate(std::string* error) {
    std::string subject;
    {
        std::lock_guard<std::mutex> lock(mu_);
        subject = status_.subject;
    }
    if (subject.empty()) {
        *error = "还没有画过，请先按住 UP 说出内容";
        return false;
    }
    return Draw(subject, error);
}

void PaintService::StartPlan(const std::string& subject) {
    ChatPlatform* platform;
    ChatConfig config;
    std::string key;
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_.phase = Phase::Planning;
        platform = platform_.get();
        config = chat_config_;
        key = api_key_;
    }
    ChatPlatform::HttpRequest request;
    request.url = config.url;
    request.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + key}};
    request.body = BuildPlanRequestJson(config, subject);
    platform->Send(std::move(request), [this, subject](int status, std::string body, std::string transport_error) {
        OnPlanned(subject, status, std::move(body), std::move(transport_error));
    });
}

// The planner only improves the prompt: if it fails for any reason the picture is still painted from the subject alone.
void PaintService::OnPlanned(const std::string& subject, int status, std::string body, std::string transport_error) {
    std::string raw, error, description;
    if (transport_error.empty() && status == 200 && ParseResponse(body, &raw, &error) && CleanPlan(raw, &description)) {
        StartGenerate(subject, ImagePrompt(description));
    } else {
        StartGenerate(subject, FallbackPrompt(subject));
    }
}

void PaintService::StartGenerate(const std::string& subject, const std::string& prompt) {
    ChatPlatform* platform;
    PaintConfig config;
    std::string key;
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_.phase = Phase::Generating;
        platform = platform_.get();
        config = paint_config_;
        key = api_key_;
    }
    ChatPlatform::HttpRequest request;
    request.url = config.url;
    request.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + key}};
    request.body = BuildImageRequestJson(config, prompt);
    (void)platform;
    // A handshake that the server drops (e.g. mbedtls -0x7280 on a flaky link) is worth one more try.
    SendWithRetry(std::move(request), 1, [this, subject](int status, std::string body, std::string transport_error) {
        OnGenerated(subject, status, std::move(body), std::move(transport_error));
    });
}

void PaintService::SendWithRetry(ChatPlatform::HttpRequest request, int retries, ChatPlatform::DoneCallback done) {
    ChatPlatform* platform;
    int delay_ms;
    {
        std::lock_guard<std::mutex> lock(mu_);
        platform = platform_.get();
        delay_ms = retry_delay_ms_;
    }
    ChatPlatform::HttpRequest again = request;  // kept for the retry (small: a JSON body or just a URL)
    platform->Send(std::move(request), [this, again = std::move(again), retries, done = std::move(done), delay_ms](
                                           int status, std::string body, std::string transport_error) mutable {
        if (!transport_error.empty() && retries > 0) {
            // Runs on the platform's request task, never the UI: sleeping here is harmless.
            if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            SendWithRetry(std::move(again), retries - 1, std::move(done));
            return;
        }
        done(status, std::move(body), std::move(transport_error));
    });
}

void PaintService::SetRetryDelayMs(int ms) {
    std::lock_guard<std::mutex> lock(mu_);
    retry_delay_ms_ = ms;
}

void PaintService::OnGenerated(const std::string& subject, int status, std::string body, std::string transport_error) {
    if (!transport_error.empty()) return Fail("生成失败: 网络错误 " + transport_error);
    std::string url, error;
    if (!ParseImageResponse(body, &url, &error)) {
        if (status == 401) error = "API Key 无效 (401)";
        else if (status != 200 && (error == "没有收到图片" || error == "响应格式错误"))
            error = "请求失败 (HTTP " + std::to_string(status) + ")";
        return Fail("生成失败: " + FriendlyError(error));
    }
    body = std::string();  // the download below needs the memory
    StartDownload(subject, url, false);
}

// The pre-signed URL is the only thing that grants access and the picture is not secret, so when the TLS connection to the
// storage host fails the same URL is fetched over plain HTTP (no handshake at all). HTTPS is not retried first: on the device
// the failure was "No matching trusted root certificate found", which is the same every time (a retry only costs seconds).
void PaintService::StartDownload(const std::string& subject, const std::string& url, bool plain_http) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_.phase = Phase::Downloading;
    }
    ChatPlatform::HttpRequest request;  // no Authorization header
    request.method = "GET";
    request.url = url;
    request.max_response = kMaxPngBytes;
    SendWithRetry(std::move(request), plain_http ? 1 : 0,  // only a plain-HTTP network hiccup is worth a retry
                  [this, subject, url, plain_http](int status, std::string png, std::string transport_error) {
                      if (!transport_error.empty() && !plain_http && url.rfind("https://", 0) == 0) {
                          StartDownload(subject, "http://" + url.substr(8), true);
                          return;
                      }
                      OnDownloaded(subject, status, std::move(png), std::move(transport_error));
                  });
}

void PaintService::OnDownloaded(const std::string& subject, int status, std::string body, std::string transport_error) {
    if (!transport_error.empty()) return Fail("下载图片失败: " + transport_error);
    if (status != 200) return Fail("下载图片失败 (HTTP " + std::to_string(status) + ")");
    SetPhase(Phase::Converting);
    std::vector<uint8_t> image;
    std::string error;
    if (!PngToBwry(body, &image, &error)) return Fail(error);
    body = std::string();
    {
        std::lock_guard<std::mutex> lock(mu_);
        image_ = std::move(image);
        busy_ = false;
        status_.working = false;
        status_.phase = Phase::Ready;
        status_.subject = subject;
        status_.has_image = true;
        status_.saved = false;
        status_.error.clear();
        ++status_.revision;
    }
    Notify();
}

bool PaintService::SaveToAlbum(std::string* error) {
    std::vector<uint8_t> image;
    std::string subject;
    uint32_t revision;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!status_.has_image) {
            *error = "还没有图片可保存";
            return false;
        }
        if (status_.working) {
            *error = "正在绘制，请稍候";
            return false;
        }
        if (status_.saved) {
            *error = "这张图已保存到相册";
            return false;
        }
        image = image_;
        subject = status_.subject;
        revision = status_.revision;
    }
    PhotoInfo info = {};
    const uint32_t now = static_cast<uint32_t>(time(nullptr));
    snprintf(info.id, sizeof(info.id), "ai%09u%02u", static_cast<unsigned>(now % 1000000000u), static_cast<unsigned>(revision % 100u));
    std::string title = subject;  // PHOTO_TITLE_LEN is in bytes: cut on a character boundary
    if (title.size() > PHOTO_TITLE_LEN - 1) {
        title.resize(PHOTO_TITLE_LEN - 1);
        while (!title.empty() && IsUtf8Continuation(static_cast<unsigned char>(title.back()))) title.pop_back();
        if (!title.empty() && static_cast<unsigned char>(title.back()) >= 0xC0) title.pop_back();
    }
    snprintf(info.title, sizeof(info.title), "%s", title.c_str());
    snprintf(info.location, sizeof(info.location), "AI 画图");
    snprintf(info.body, sizeof(info.body), "AI 绘制 · 四色 · %s", subject.c_str());
    info.width = kWidth;
    info.height = kHeight;
    info.file_size = static_cast<uint32_t>(kImageBytes);
    info.timestamp = now;
    if (now > 0) {
        time_t t = now;
        struct tm tm_info;
        localtime_r(&t, &tm_info);
        strftime(info.date, sizeof(info.date), "%Y-%m-%d", &tm_info);
    }
    if (photo_save(&info, image.data()) != 0) {
        *error = "保存失败，相册可能已满";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (status_.revision == revision) status_.saved = true;
    }
    Notify();
    return true;
}

Status PaintService::GetStatus() {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

bool PaintService::CopyImage(std::vector<uint8_t>* out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!status_.has_image) return false;
    *out = image_;
    return true;
}

}  // namespace paint
}  // namespace ai
