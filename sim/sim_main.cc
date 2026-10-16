// Host simulator: renders a rawdraw page into a 2bpp framebuffer and replays button events.
//   sim_main --page gallery|photo_detail|weather|weather_detail --out DIR [--keys up,down,boot,...]
// Writes DIR/<page>_<step>.fb after the initial render and after every key.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common/photo_storage.h"
#include "common/weather_api.h"
#include "ui/renderers/rawdraw/photo_detail_renderer.h"
#include "ui/renderers/rawdraw/photo_gallery.h"
#include "ui/renderers/rawdraw/weather_detail_renderer.h"
#include "ui/renderers/rawdraw/weather_renderer.h"

namespace {
constexpr int kW = 400;
constexpr int kH = 300;
constexpr int kBpr = (kW * 2 + 7) >> 3;

using rawdraw::ButtonEvent;

// Synthetic 400x300 BWRY 2bpp photo so fixtures need no external files.
std::vector<uint8_t> MakePattern(int variant) {
    std::vector<uint8_t> buf(kBpr * kH, 0x55);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            int c;
            switch (variant % 3) {
                case 0: c = ((x / 40) + (y / 40)) % 4; break;
                case 1: c = ((x - 200) * (x - 200) + (y - 150) * (y - 150) < 90 * 90) ? 3 : ((y / 20) % 2 ? 1 : 2); break;
                default: c = (x + y) / 50 % 4; break;
            }
            uint8_t& b = buf[y * kBpr + (x >> 2)];
            const int shift = 6 - (x & 3) * 2;
            b = static_cast<uint8_t>((b & ~(3 << shift)) | (c << shift));
        }
    }
    return buf;
}

void SeedPhotos() {
    const char* titles[] = {"海边日落", "山间小路", "城市夜景"};
    for (int i = 0; i < 3; ++i) {
        PhotoInfo info = {};
        snprintf(info.id, sizeof(info.id), "demo%d", i);
        snprintf(info.title, sizeof(info.title), "%s", titles[i]);
        snprintf(info.date, sizeof(info.date), "2026-05-2%d", i + 1);
        snprintf(info.location, sizeof(info.location), "示例地点");
        snprintf(info.body, sizeof(info.body), "模拟器生成的示例照片 %d · 2BP 四色", i + 1);
        info.width = kW;
        info.height = kH;
        auto data = MakePattern(i);
        info.file_size = static_cast<uint32_t>(data.size());
        photo_save(&info, data.data());
    }
}

WeatherData MakeWeather() {
    WeatherData d;
    d.city = "杭州";
    d.temp = "25";
    d.temp_int = 25;
    d.feels_like = "27";
    d.weather_icon = "101";
    d.weather_text = "多云";
    d.wind_dir = "东南风";
    d.wind_scale = "3";
    d.humidity = "45";
    d.update_time = "14:30";
    d.air_quality = "优";
    d.air_aqi = 38;
    const char* labels[] = {"今天", "明天", "后天"};
    const char* texts[] = {"多云", "小雨", "晴"};
    const char* icons[] = {"101", "305", "100"};
    for (int i = 0; i < 3; ++i) {
        WeatherForecastDay f;
        f.label = labels[i];
        f.weather_text = texts[i];
        f.icon_code = icons[i];
        f.temp_min = 18 + i;
        f.temp_max = 27 - i * 2;
        d.forecast.push_back(f);
    }
    return d;
}

bool ParseKey(const std::string& k, ButtonEvent* out) {
    static const struct { const char* name; ButtonEvent::Type type; } kKeys[] = {
        {"up", ButtonEvent::kUpClick},         {"down", ButtonEvent::kDownClick},
        {"up2", ButtonEvent::kUpDoubleClick},  {"down2", ButtonEvent::kDownDoubleClick},
        {"uplong", ButtonEvent::kUpLongPress}, {"downlong", ButtonEvent::kDownLongPress},
        {"boot", ButtonEvent::kBootClick},     {"boot2", ButtonEvent::kBootDoubleClick},
        {"bootlong", ButtonEvent::kBootLongPress},
    };
    for (const auto& e : kKeys) {
        if (k == e.name) {
            out->type = e.type;
            return true;
        }
    }
    return false;
}

void Dump(const std::string& dir, const std::string& page, int step, const std::vector<uint8_t>& fb) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%02d.fb", dir.c_str(), page.c_str(), step);
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fwrite(fb.data(), 1, fb.size(), f);
    fclose(f);
    printf("%s\n", path);
}
}  // namespace

int main(int argc, char** argv) {
    std::string page = "gallery", out = ".", keys;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--page")) page = argv[i + 1];
        else if (!strcmp(argv[i], "--out")) out = argv[i + 1];
        else if (!strcmp(argv[i], "--keys")) keys = argv[i + 1];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }

    SeedPhotos();
    // Init() resets renderer state, so data is injected afterwards (as the UI manager does on page entry).
    std::unique_ptr<rawdraw::PageRenderer> r;
    std::function<void()> feed = [] {};
    if (page == "gallery") {
        auto g = std::make_unique<rawdraw::PhotoGalleryRenderer>();
        feed = [p = g.get()] { p->RefreshPhotoList(); };
        r = std::move(g);
    } else if (page == "photo_detail") {
        r = std::make_unique<rawdraw::PhotoDetailRenderer>();
    } else if (page == "weather") {
        auto w = std::make_unique<rawdraw::WeatherRenderer>();
        feed = [p = w.get()] { p->Update(MakeWeather()); };
        r = std::move(w);
    } else if (page == "weather_detail") {
        auto w = std::make_unique<rawdraw::WeatherDetailRenderer>();
        feed = [p = w.get()] { p->Update(MakeWeather()); };
        r = std::move(w);
    } else {
        fprintf(stderr, "unknown page %s\n", page.c_str());
        return 2;
    }

    std::vector<uint8_t> fb(kBpr * kH, 0x55);
    r->Init(kW, kH);
    feed();
    r->Render(fb.data(), kW, kH);
    Dump(out, page, 0, fb);

    std::stringstream ss(keys);
    std::string k;
    int step = 1;
    while (std::getline(ss, k, ',')) {
        ButtonEvent ev;
        if (!ParseKey(k, &ev)) {
            fprintf(stderr, "unknown key %s\n", k.c_str());
            return 2;
        }
        const bool consumed = r->HandleInput(ev);
        r->Render(fb.data(), kW, kH);
        printf("key=%s consumed=%d\n", k.c_str(), consumed);
        Dump(out, page, step++, fb);
    }
    return 0;
}
