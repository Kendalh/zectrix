// Shared fixtures for the simulator binaries: fake photos, weather data, key-name parsing.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/photo_storage.h"
#include "common/weather_api.h"
#include "common/weather_parse.h"
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

// Real-shape Open-Meteo responses, fed through the firmware's own parser (common/weather_parse.cc).
inline const char* SampleForecastJson() {
    return R"JSON({"latitude":30.29,"longitude":120.16,"timezone":"Asia/Shanghai",
"current":{"time":"2026-10-16T14:30","interval":900,"temperature_2m":24.6,"apparent_temperature":26.8,
 "relative_humidity_2m":45,"weather_code":2,"pressure_msl":1011.7,"wind_speed_10m":14.2,"wind_direction_10m":135},
"hourly":{"time":["2026-10-16T14:00","2026-10-16T15:00","2026-10-16T16:00","2026-10-16T17:00","2026-10-16T18:00","2026-10-16T19:00","2026-10-16T20:00","2026-10-16T21:00","2026-10-16T22:00","2026-10-16T23:00","2026-10-17T00:00","2026-10-17T01:00","2026-10-17T02:00","2026-10-17T03:00","2026-10-17T04:00","2026-10-17T05:00"],
 "temperature_2m":[24.6,24.1,23.5,22.2,21.0,20.4,19.8,19.1,18.6,18.2,17.9,17.5,17.2,17.0,16.8,16.6],
 "weather_code":[2,2,3,3,61,61,61,3,3,1,0,0,0,0,0,0]},
"daily":{"time":["2026-10-16","2026-10-17","2026-10-18","2026-10-19","2026-10-20","2026-10-21","2026-10-22"],
 "weather_code":[2,61,63,3,0,45,95],
 "temperature_2m_max":[27.4,24.9,22.0,23.1,26.6,25.8,21.3],
 "temperature_2m_min":[18.2,16.7,15.0,14.2,15.9,17.4,16.1],
 "uv_index_max":[5.8,3.1,2.0,3.4,6.9,4.2,2.5]}})JSON";
}

inline const char* SampleAirJson() { return R"JSON({"current":{"time":"2026-10-16T14:00","us_aqi":38}})JSON"; }

inline WeatherData MakeWeather(const std::string& city = "杭州") {
    WeatherData d;
    d.city = city;
    ParseOpenMeteoForecast(SampleForecastJson(), &d);
    ParseOpenMeteoAir(SampleAirJson(), &d);
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
