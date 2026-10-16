// Host stand-in for common/weather_api.cc. Network fetching is replaced by data the
// simulator injects directly into the renderers; only the pure mapping helper lives here.
// Keep ParseWeatherIcon in sync with firmware/main/common/weather_api.cc.
#include "common/weather_api.h"

#include <cstring>

WeatherIcon ParseWeatherIcon(const char* text) {
    if (!text || !text[0]) return WeatherIcon::Unknown;
    if (strstr(text, "晴") != nullptr) return WeatherIcon::Sunny;
    if (strstr(text, "多云") != nullptr) return WeatherIcon::Cloudy;
    if (strstr(text, "阴") != nullptr) return WeatherIcon::Overcast;
    if (strstr(text, "雨") != nullptr) return WeatherIcon::Rain;
    if (strstr(text, "雪") != nullptr) return WeatherIcon::Snow;
    if (strstr(text, "雾") != nullptr || strstr(text, "霾") != nullptr || strstr(text, "沙尘") != nullptr)
        return WeatherIcon::Fog;
    return WeatherIcon::Unknown;
}

// The device refetches over HTTP here; the simulator has no network, so report failure.
bool weather_api_fetch_now() { return false; }
