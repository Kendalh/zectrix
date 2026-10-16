// Shared fixtures for the simulator binaries: fake photos, weather data, key-name parsing.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/photo_storage.h"
#include "common/weather_api.h"
#include "ui/renderers/rawdraw/page_renderer.h"

namespace sim {
inline constexpr int kW = 400;
inline constexpr int kH = 300;
inline constexpr int kBpr = (kW * 2 + 7) >> 3;

using rawdraw::ButtonEvent;

// Synthetic 400x300 BWRY 2bpp photo so fixtures need no external files.
inline std::vector<uint8_t> MakePattern(int variant) {
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

inline void SeedPhotos() {
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

inline WeatherData MakeWeather() {
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

inline bool ParseKey(const std::string& k, ButtonEvent* out) {
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

}  // namespace sim
