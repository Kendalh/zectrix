// Unit tests for common/png_decode.cc and common/ai_paint.cc: PNG decoding against reference pixels, the style prompt,
// request/response helpers, PNG -> BWRY conversion, and the painter service (scripted platform instead of the network).
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <cJSON.h>

#include "common/ai_paint.h"
#include "common/photo_storage.h"
#include "common/png_decode.h"

static int g_failed = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                     \
        }                                                                   \
    } while (0)

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string DataPath(const char* name) {
    const char* dir = getenv("TEST_DATA_DIR");
    return std::string(dir ? dir : "tests/data") + "/" + name;
}

// ---- tiny PNG encoder (stored deflate blocks) for synthetic images ----
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
void Chunk(std::string* png, const char* type, const std::string& body) {
    Put32(png, static_cast<uint32_t>(body.size()));
    std::string tb = std::string(type, 4) + body;
    png->append(tb);
    Put32(png, Crc32(tb));
}
// color(x, y) -> {r, g, b}
template <typename F>
std::string MakePng(int w, int h, F color) {
    std::string raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            auto c = color(x, y);
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
    uint32_t a = 1, b = 0;  // Adler-32 (ignored by the decoder, but keep the stream valid)
    for (unsigned char ch : raw) {
        a = (a + ch) % 65521;
        b = (b + a) % 65521;
    }
    Put32(&z, (b << 16) | a);
    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    Put32(&ihdr, w);
    Put32(&ihdr, h);
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);
    Chunk(&png, "IHDR", ihdr);
    Chunk(&png, "IDAT", z);
    Chunk(&png, "IEND", "");
    return png;
}

void TestPngDecodeMatchesReference() {
    for (const char* name : {"sample_rgb", "sample_pal", "sample_rgba", "sample_gray"}) {
        const std::string png = ReadFile(DataPath((std::string(name) + ".png").c_str()));
        const std::string ref = ReadFile(DataPath((std::string(name) + ".rgb").c_str()));
        CHECK(!png.empty() && ref.size() == 96 * 72 * 3);
        png::Image img;
        std::string err;
        const bool ok = png::Decode(reinterpret_cast<const uint8_t*>(png.data()), png.size(), &img, &err);
        if (!ok) fprintf(stderr, "  %s: %s\n", name, err.c_str());
        CHECK(ok);
        CHECK(img.width == 96 && img.height == 72);
        CHECK(img.rgb.size() == ref.size());
        int diff = 0;
        for (size_t i = 0; i < ref.size() && i < img.rgb.size(); ++i)
            if (abs(static_cast<int>(img.rgb[i]) - static_cast<int>(static_cast<unsigned char>(ref[i]))) > 1) ++diff;
        if (diff) fprintf(stderr, "  %s: %d channel values differ\n", name, diff);
        CHECK(diff == 0);  // RGBA is composited on white with integer maths: +-1 allowed above
    }
}

void TestPngDecodeRejects() {
    png::Image img;
    std::string err;
    CHECK(!png::Decode(reinterpret_cast<const uint8_t*>("hello"), 5, &img, &err));
    std::string png = ReadFile(DataPath("sample_rgb.png"));
    CHECK(!png::Decode(reinterpret_cast<const uint8_t*>(png.data()), png.size() / 2, &img, &err));  // truncated
    png[40] ^= 0x5A;  // damage the compressed data: an error or garbage pixels, never a crash
    png::Decode(reinterpret_cast<const uint8_t*>(png.data()), png.size(), &img, &err);
}

void TestSubjectNormalization() {
    using namespace ai::paint;
    CHECK(NormalizeSubject("  。上海外滩的夜景！ ") == "上海外滩的夜景");
    CHECK(NormalizeSubject("Mount Fuji.") == "Mount Fuji");
    CHECK(NormalizeSubject("") == "");
    CHECK(NormalizeSubject("，。 ") == "");
    CHECK(NormalizeSubject("一\n二") == "一 二");
    std::string long_text;
    for (int i = 0; i < 100; ++i) long_text += "画";
    CHECK(NormalizeSubject(long_text).size() == kMaxSubjectChars * 3);  // cut by characters, never mid-character
}

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

void TestPrompts() {
    using namespace ai::paint;
    // The planner's system prompt is where the scene is decided: time of day picks the sky and the celestial disc.
    const std::string sys = PlannerSystemPrompt();
    CHECK(Has(sys, "pure black, pure white, pure yellow and pure red"));
    CHECK(Has(sys, "sunset or sunrise = a giant red sun disc"));
    CHECK(Has(sys, "night = a large white or pale-yellow full moon"));
    CHECK(Has(sys, "Never draw a sun at night and never draw a moon in daytime"));
    CHECK(Has(sys, "rain, snow, fog or heavy cloud = an overcast sky"));
    CHECK(Has(sys, "NO sun, NO moon and NO circular disc"));
    CHECK(Has(sys, "if none is given, use sunset"));
    CHECK(Has(sys, "ONE English paragraph"));

    // The shared poster style must not force a sun: a night scene's moon has to survive into the final prompt.
    const std::string planned = ImagePrompt("A large white full moon over the Golden Gate Bridge at night.");
    CHECK(planned.find("Flat vector screen-print poster.") == 0);
    CHECK(Has(planned, "full moon over the Golden Gate Bridge"));
    CHECK(!Has(planned, "red sun"));
    CHECK(!Has(planned, "sun disc"));
    CHECK(Has(planned, "STRICT four-color palette only: pure black, pure white, pure yellow, pure red"));

    // Without the planner the neutral fallback still lets night mean moon.
    const std::string fallback = FallbackPrompt("夜晚的金门大桥");
    CHECK(fallback.find("Flat vector screen-print poster of 夜晚的金门大桥.") == 0);
    CHECK(Has(fallback, "white moon at night"));
    CHECK(Has(fallback, "sunset (the default)"));
    CHECK(Has(NegativePrompt(), "photograph"));

    cJSON* root = cJSON_Parse(BuildPlanRequestJson(ai::ChatConfig(), "夜晚的金门大桥").c_str());
    CHECK(root != nullptr);
    CHECK(std::string(cJSON_GetObjectItem(root, "model")->valuestring) == "qwen-flash");
    cJSON* messages = cJSON_GetObjectItem(root, "messages");
    CHECK(cJSON_GetArraySize(messages) == 2);
    CHECK(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "role")->valuestring) == "system");
    CHECK(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "content")->valuestring) == sys);
    CHECK(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 1), "content")->valuestring) == "夜晚的金门大桥");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(root, "stream")));
    CHECK(cJSON_GetObjectItem(root, "enable_search") == nullptr);  // no web search for a prompt
    cJSON_Delete(root);

    std::string plan;
    CHECK(CleanPlan("  \"A **large** moon\n over the bridge, black towers.\"  ", &plan));
    CHECK(plan == "A large moon over the bridge, black towers.");
    CHECK(!CleanPlan("OK", &plan) && !CleanPlan("", &plan));
    std::string long_plan(3000, 'x');
    CHECK(CleanPlan(long_plan, &plan) && plan.size() == 1000);
}

void TestFriendlyError() {
    using namespace ai::paint;
    CHECK(FriendlyError("Requests rate limit exceeded, please try again later. For details, see: https://x") == "请求太频繁，请稍后再试");
    CHECK(FriendlyError("Arrearage: Access denied") == "额度不足或账号欠费");
    CHECK(FriendlyError("something unexpected") == "something unexpected");
}

void TestRequestAndResponse() {
    using namespace ai::paint;
    PaintConfig cfg;
    cJSON* root = cJSON_Parse(BuildImageRequestJson(cfg, "A cat under cherry blossoms 猫").c_str());
    CHECK(root != nullptr);
    CHECK(std::string(cJSON_GetObjectItem(root, "model")->valuestring) == "qwen-image-2.0-pro");
    cJSON* text = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "input"), "messages"), 0), "content"), 0), "text");
    CHECK(text && std::string(text->valuestring) == "A cat under cherry blossoms 猫");  // sent as is
    cJSON* params = cJSON_GetObjectItem(root, "parameters");
    CHECK(std::string(cJSON_GetObjectItem(params, "size")->valuestring) == "640*480");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(params, "prompt_extend")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(params, "watermark")));
    cJSON_Delete(root);

    std::string url, error;
    CHECK(ParseImageResponse(R"({"output":{"choices":[{"message":{"content":[{"image":"https://x/y.png?a=b"}]}}]}})", &url, &error));
    CHECK(url == "https://x/y.png?a=b");
    CHECK(!ParseImageResponse(R"({"code":"InvalidApiKey","message":"Invalid API-key provided."})", &url, &error));
    CHECK(error == "Invalid API-key provided.");
    CHECK(!ParseImageResponse("not json", &url, &error) && error == "响应格式错误");
    CHECK(!ParseImageResponse(R"({"output":{"choices":[]}})", &url, &error) && error == "没有收到图片");
}

// Pixel (x, y) of a BWRY buffer.
int Ink(const std::vector<uint8_t>& img, int x, int y) {
    const size_t p = static_cast<size_t>(y) * 400 + x;
    return (img[p >> 2] >> (6 - (p & 3) * 2)) & 3;
}

void TestPngToBwry() {
    using namespace ai::paint;
    // 800x600: paper frame, left third red, middle white, right third black, a yellow band at the bottom.
    const std::string png = MakePng(800, 600, [](int x, int y) -> std::array<int, 3> {
        if (x < 24 || y < 18 || x >= 776 || y >= 582) return {250, 245, 230};  // frame
        if (y > 500) return {255, 255, 0};
        if (x < 280) return {255, 0, 0};
        if (x < 520) return {255, 255, 255};
        return {0, 0, 0};
    });
    std::vector<uint8_t> out;
    std::string error;
    CHECK(PngToBwry(png, &out, &error));
    CHECK(out.size() == kImageBytes);
    CHECK(Ink(out, 50, 100) == 3);    // red
    CHECK(Ink(out, 200, 100) == 1);   // white
    CHECK(Ink(out, 350, 100) == 0);   // black
    CHECK(Ink(out, 200, 280) == 2);   // yellow
    CHECK(!PngToBwry("junk", &out, &error) && error.find("图片解码失败") == 0);
}

// Captures requests; the test completes them by hand.
class ScriptedPlatform : public ai::ChatPlatform {
public:
    struct Sent {
        HttpRequest request;
        DoneCallback done;
    };
    void Send(HttpRequest r, DoneCallback d) override { sent.push_back({std::move(r), std::move(d)}); }
    std::string LoadApiKey() override { return key; }
    void SaveApiKey(const std::string& k) override { key = k; }
    std::vector<Sent> sent;
    std::string key = "sk-test";
};

std::string ImageJson(const std::string& url) {
    return R"({"output":{"choices":[{"message":{"content":[{"image":")" + url + R"("}]}}]}})";
}

std::string TranscriptJson(const std::string& text) {
    return R"({"choices":[{"message":{"content":")" + text + R"("}}]})";
}

const std::string& SamplePng() {
    static const std::string png = MakePng(800, 600, [](int x, int y) -> std::array<int, 3> {
        return x < 400 ? std::array<int, 3>{255, 0, 0} : std::array<int, 3>{0, 0, 0};
    });
    return png;
}

// Steps through the requests a painting sends, in order: plan (chat), image, download.
struct Script {
    explicit Script(ScriptedPlatform* platform) : p(platform) {}
    ScriptedPlatform::Sent& Next() { return p->sent[n++]; }
    bool Pending() const { return n < p->sent.size(); }
    void Plan(const std::string& description) { Next().done(200, TranscriptJson(description), ""); }
    void ImageAndDownload(const std::string& url = "https://oss.example/a.png") {
        Next().done(200, ImageJson(url), "");
        Next().done(200, SamplePng(), "");
    }
    ScriptedPlatform* p;
    size_t n = 0;
};

const char kScene[] = "A large white full moon over the Golden Gate Bridge at night, black towers with thin white lines.";

void TestServiceDrawRegenerateSave() {
    auto platform = std::make_unique<ScriptedPlatform>();
    ScriptedPlatform* p = platform.get();
    Script script(p);
    ai::paint::PaintService svc;
    int changes = 0;
    svc.SetOnChange([&] { ++changes; });
    svc.SetRetryDelayMs(0);
    svc.Start(std::move(platform));
    std::string error;

    CHECK(!svc.Regenerate(&error));  // nothing drawn yet
    CHECK(!svc.SaveToAlbum(&error));
    CHECK(svc.Draw("  夜晚的金门大桥。 ", &error));
    CHECK(svc.GetStatus().working && svc.GetStatus().phase == ai::paint::Phase::Planning);
    CHECK(!svc.Draw("另一个", &error) && error == "正在绘制，请稍候");  // one at a time

    // 1. planning: the chat model gets the system prompt and the subject, with the key
    CHECK(p->sent.size() == 1);
    CHECK(p->sent[0].request.url.find("chat/completions") != std::string::npos);
    bool has_auth = false;
    for (auto& h : p->sent[0].request.headers) has_auth = has_auth || (h.first == "Authorization" && h.second == "Bearer sk-test");
    CHECK(has_auth);
    CHECK(Has(p->sent[0].request.body, "夜晚的金门大桥") && Has(p->sent[0].request.body, "Never draw a sun at night"));
    script.Plan(kScene);

    // 2. image: the planner's scene is the prompt, in the shared style, with no forced sun
    CHECK(svc.GetStatus().phase == ai::paint::Phase::Generating);
    CHECK(p->sent.size() == 2);
    CHECK(p->sent[1].request.url.find("multimodal-generation") != std::string::npos);
    CHECK(Has(p->sent[1].request.body, "full moon over the Golden Gate Bridge"));
    CHECK(!Has(p->sent[1].request.body, "red sun"));
    CHECK(Has(p->sent[1].request.body, "STRICT four-color palette only"));
    script.Next().done(200, ImageJson("https://oss.example/a.png?sig=1"), "");

    // 3. download: pre-signed URL, big cap, no key
    CHECK(p->sent.size() == 3);
    CHECK(p->sent[2].request.method == "GET" && p->sent[2].request.url == "https://oss.example/a.png?sig=1");
    CHECK(p->sent[2].request.max_response >= 2 * 1024 * 1024);
    bool leaks_key = false;
    for (auto& h : p->sent[2].request.headers) leaks_key = leaks_key || h.first == "Authorization";
    CHECK(!leaks_key);  // the pre-signed URL must not get the API key
    script.Next().done(200, SamplePng(), "");

    ai::paint::Status st = svc.GetStatus();
    CHECK(st.phase == ai::paint::Phase::Ready && st.has_image && !st.working && !st.saved);
    CHECK(st.subject == "夜晚的金门大桥" && st.revision == 1);
    std::vector<uint8_t> image;
    CHECK(svc.CopyImage(&image) && image.size() == ai::paint::kImageBytes);
    CHECK(Ink(image, 10, 10) == 3 && Ink(image, 390, 10) == 0);

    // Save: once per picture, shows up in the album as a 400x300 2 bpp photo.
    const int before = photo_get_count();
    CHECK(svc.SaveToAlbum(&error));
    CHECK(photo_get_count() == before + 1);
    PhotoInfo info;
    bool found = false;
    for (int i = 0; i < photo_get_count(); ++i)
        if (photo_get_by_index(i, &info) == 0 && std::string(info.title) == "夜晚的金门大桥") {
            found = true;
            CHECK(info.width == 400 && info.height == 300 && info.file_size == 30000);
        }
    CHECK(found);
    CHECK(svc.GetStatus().saved);
    CHECK(!svc.SaveToAlbum(&error) && photo_get_count() == before + 1);  // not twice

    // Regenerate plans again (a fresh scene each time); a failure keeps the old picture.
    CHECK(svc.Regenerate(&error));
    CHECK(p->sent.size() == 4 && Has(p->sent[3].request.body, "夜晚的金门大桥"));
    script.Plan(kScene);
    script.Next().done(200, "", "timeout");  // the image request is retried once...
    script.Next().done(200, "", "timeout");  // ...and then the error stands
    st = svc.GetStatus();
    CHECK(st.phase == ai::paint::Phase::Error && st.has_image && !st.working && Has(st.error, "timeout"));
    CHECK(st.revision == 1 && svc.CopyImage(&image));
    CHECK(svc.Regenerate(&error));
    script.Plan(kScene);
    script.ImageAndDownload("https://oss.example/b.png");
    st = svc.GetStatus();
    CHECK(st.phase == ai::paint::Phase::Ready && st.revision == 2 && !st.saved);
    CHECK(changes > 5);
}

void TestPlannerFailureFallsBack() {
    // Whatever goes wrong in the planning step, the picture is still painted from the subject alone.
    struct Case {
        int status;
        std::string body, transport;
    };
    const Case cases[] = {{0, "", "timeout"},
                          {500, "oops", ""},
                          {200, "not json", ""},
                          {200, TranscriptJson("ok"), ""},  // too short to be a scene
                          {401, R"({"error":{"message":"bad key"}})", ""}};
    for (const Case& c : cases) {
        auto platform = std::make_unique<ScriptedPlatform>();
        ScriptedPlatform* p = platform.get();
        Script script(p);
        ai::paint::PaintService svc;
        svc.SetRetryDelayMs(0);
    svc.Start(std::move(platform));
        std::string error;
        CHECK(svc.Draw("夜晚的金门大桥", &error));
        script.Next().done(c.status, c.body, c.transport);
        CHECK(p->sent.size() == 2);  // still goes on to the image model
        CHECK(Has(p->sent[1].request.body, "Flat vector screen-print poster of 夜晚的金门大桥."));
        CHECK(Has(p->sent[1].request.body, "white moon at night"));
        script.Next().done(200, ImageJson("https://oss/x.png"), "");
        script.Next().done(200, SamplePng(), "");
        CHECK(svc.GetStatus().phase == ai::paint::Phase::Ready);
    }
}

void TestServiceFailures() {
    auto platform = std::make_unique<ScriptedPlatform>();
    ScriptedPlatform* p = platform.get();
    Script script(p);
    ai::paint::PaintService svc;
    svc.SetRetryDelayMs(0);
    svc.Start(std::move(platform));
    std::string error;
    p->key.clear();
    CHECK(!svc.Draw("猫", &error) && error == "请先设置百炼 API Key");
    CHECK(p->sent.empty());
    p->key = "sk-test";

    CHECK(svc.Draw("猫", &error));
    script.Plan(kScene);
    script.Next().done(401, R"({"code":"InvalidApiKey","message":"Invalid API-key provided."})", "");
    CHECK(svc.GetStatus().phase == ai::paint::Phase::Error && svc.GetStatus().error == "生成失败: API Key 无效 (401)");
    CHECK(!svc.GetStatus().has_image);

    CHECK(svc.Draw("猫", &error));
    script.Plan(kScene);
    script.Next().done(200, ImageJson("https://oss/x.png"), "");
    script.Next().done(403, "denied", "");
    CHECK(svc.GetStatus().error == "下载图片失败 (HTTP 403)");

    CHECK(svc.Draw("猫", &error));
    script.Plan(kScene);
    script.Next().done(200, ImageJson("https://oss/x.png"), "");
    script.Next().done(200, "this is not a png", "");
    CHECK(svc.GetStatus().error.find("图片解码失败") == 0);
    CHECK(svc.Draw("猫", &error));  // an error does not block the next try
}

void TestNetworkRetries() {
    std::string error;
    auto make = [](ScriptedPlatform** out) {
        auto platform = std::make_unique<ScriptedPlatform>();
        *out = platform.get();
        auto svc = std::make_unique<ai::paint::PaintService>();
        svc->SetRetryDelayMs(0);
        svc->Start(std::move(platform));
        return svc;
    };
    {   // a dropped handshake on the image request: tried again, then it works
        ScriptedPlatform* p;
        auto svc = make(&p);
        Script script(p);
        CHECK(svc->Draw("猫", &error));
        script.Plan(kScene);
        script.Next().done(0, "", "ESP_ERR_HTTP_CONNECT");
        CHECK(p->sent.size() == 3 && p->sent[2].request.url == p->sent[1].request.url);  // the same request again
        CHECK(p->sent[2].request.body == p->sent[1].request.body);
        script.ImageAndDownload();
        CHECK(svc->GetStatus().phase == ai::paint::Phase::Ready);
    }
    {   // the download over TLS fails (once is enough: a certificate error never fixes itself): same URL over plain http
        ScriptedPlatform* p;
        auto svc = make(&p);
        Script script(p);
        CHECK(svc->Draw("猫", &error));
        script.Plan(kScene);
        script.Next().done(200, ImageJson("https://oss.example/a.png?sig=1"), "");
        script.Next().done(0, "", "ESP_ERR_HTTP_CONNECT");
        CHECK(p->sent.size() == 4);
        CHECK(p->sent[2].request.url == "https://oss.example/a.png?sig=1");
        CHECK(p->sent[3].request.url == "http://oss.example/a.png?sig=1");  // straight to http, no https retry
        CHECK(svc->GetStatus().phase == ai::paint::Phase::Downloading && svc->GetStatus().working);
        script.Next().done(200, SamplePng(), "");
        CHECK(svc->GetStatus().phase == ai::paint::Phase::Ready && svc->GetStatus().has_image);
    }
    {   // a network hiccup on the plain-http attempt is retried once; then the error names the step
        ScriptedPlatform* p;
        auto svc = make(&p);
        Script script(p);
        CHECK(svc->Draw("猫", &error));
        script.Plan(kScene);
        script.Next().done(200, ImageJson("https://oss.example/a.png"), "");
        for (int i = 0; i < 3; ++i) script.Next().done(0, "", "ESP_ERR_HTTP_CONNECT");
        CHECK(p->sent.size() == 5);  // plan, image, https, http, http retry: no endless loop
        CHECK(p->sent[3].request.url == "http://oss.example/a.png" && p->sent[4].request.url == p->sent[3].request.url);
        CHECK(svc->GetStatus().phase == ai::paint::Phase::Error);
        CHECK(svc->GetStatus().error == "下载图片失败: ESP_ERR_HTTP_CONNECT");
    }
    {   // the image request keeps failing: error names the step
        ScriptedPlatform* p;
        auto svc = make(&p);
        Script script(p);
        CHECK(svc->Draw("猫", &error));
        script.Plan(kScene);
        script.Next().done(0, "", "ESP_ERR_HTTP_CONNECT");
        script.Next().done(0, "", "ESP_ERR_HTTP_CONNECT");
        CHECK(p->sent.size() == 3);
        CHECK(svc->GetStatus().error == "生成失败: 网络错误 ESP_ERR_HTTP_CONNECT");
    }
    {   // HTTP-level failures are answers, not network errors: never retried
        ScriptedPlatform* p;
        auto svc = make(&p);
        Script script(p);
        CHECK(svc->Draw("猫", &error));
        script.Plan(kScene);
        script.Next().done(500, "oops", "");
        CHECK(p->sent.size() == 2);
        CHECK(svc->GetStatus().phase == ai::paint::Phase::Error);
    }
    {   // the planner is optional: a network error there goes straight to the fallback prompt, no retry delay
        ScriptedPlatform* p;
        auto svc = make(&p);
        Script script(p);
        CHECK(svc->Draw("猫", &error));
        script.Next().done(0, "", "ESP_ERR_HTTP_CONNECT");
        CHECK(p->sent.size() == 2 && Has(p->sent[1].request.body, "Flat vector screen-print poster of 猫."));
    }
}

void TestServiceVoice() {
    auto platform = std::make_unique<ScriptedPlatform>();
    ScriptedPlatform* p = platform.get();
    Script script(p);
    ai::paint::PaintService svc;
    svc.SetRetryDelayMs(0);
    svc.Start(std::move(platform));
    std::string error;
    CHECK(svc.VoiceReady(&error));

    char path[] = "/tmp/test_paint_wav_XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    CHECK(svc.AskVoice(path, &error));
    CHECK(svc.GetStatus().phase == ai::paint::Phase::Transcribing && svc.GetStatus().working);
    CHECK(!svc.VoiceReady(&error) && error == "正在绘制，请稍候");
    CHECK(p->sent.size() == 1 && p->sent[0].request.body_file == path);
    CHECK(Has(p->sent[0].request.body_prefix, "qwen3-asr-flash"));
    script.Next().done(200, TranscriptJson("上海外滩的夜景。"), "");
    CHECK(svc.GetStatus().phase == ai::paint::Phase::Planning);
    CHECK(p->sent.size() == 2);
    CHECK(!Has(p->sent[1].request.body, "夜景。"));  // the trailing 。 was dropped
    CHECK(Has(p->sent[1].request.body, "上海外滩的夜景"));
    script.Plan(kScene);
    script.ImageAndDownload();
    CHECK(svc.GetStatus().phase == ai::paint::Phase::Ready && svc.GetStatus().subject == "上海外滩的夜景");

    // While working the visible subject is still the previous one: the page must not show it (see AiPaintRenderer).
    CHECK(svc.AskVoice(path, &error));
    CHECK(svc.GetStatus().working && svc.GetStatus().subject == "上海外滩的夜景");
    // Silence comes back as a lone "嗯。"
    script.Next().done(200, TranscriptJson("嗯。"), "");
    CHECK(svc.GetStatus().phase == ai::paint::Phase::Error && svc.GetStatus().error == "没听清，请再说一次");
    CHECK(svc.GetStatus().has_image);  // the previous picture stays
    CHECK(svc.AskVoice(path, &error));
    script.Next().done(0, "", "connection reset");
    CHECK(svc.GetStatus().error.find("语音识别失败") == 0);
    svc.SetError("没有录到声音");
    CHECK(svc.GetStatus().error == "没有录到声音");
    remove(path);
}

}  // namespace

int main() {
    TestPngDecodeMatchesReference();
    TestPngDecodeRejects();
    TestSubjectNormalization();
    TestPrompts();
    TestFriendlyError();
    TestRequestAndResponse();
    TestPngToBwry();
    TestServiceDrawRegenerateSave();
    TestPlannerFailureFallsBack();
    TestServiceFailures();
    TestNetworkRetries();
    TestServiceVoice();
    if (g_failed) {
        fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("test_ai_paint: all passed\n");
    return 0;
}
