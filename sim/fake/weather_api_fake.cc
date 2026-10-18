// Host stand-in for common/weather_api.cc. There is no network in the simulator: refreshes
// deliver the canned Open-Meteo sample (sim_fixtures.h) through the real parser, labelled with
// the configured city, and city search matches a small built-in table.
// Parsing and code mapping (common/weather_parse.cc) are the real firmware code.
#include "common/weather_api.h"

#include <cstring>
#include <mutex>

#include "../sim_fixtures.h"

namespace {
std::mutex g_mutex;
WeatherConfig g_config;
WeatherCallback g_callback;
WeatherData g_last;
bool g_has_last = false;
bool g_started = false;

struct Known { const char* name; const char* admin; const char* lat; const char* lon; };
const Known kCities[] = {
    {"北京", "北京", "39.9042", "116.4074"}, {"上海", "上海", "31.2304", "121.4737"},
    {"杭州", "浙江", "30.2936", "120.1614"}, {"广州", "广东", "23.1291", "113.2644"},
    {"深圳", "广东", "22.5431", "114.0579"}, {"成都", "四川", "30.5728", "104.0668"},
    {"武汉", "湖北", "30.5928", "114.3055"}, {"南京", "江苏", "32.0603", "118.7969"},
    {"西安", "陕西", "34.3416", "108.9398"}, {"重庆", "重庆", "29.5630", "106.5516"},
    {"苏州", "江苏", "31.2989", "120.5853"}, {"天津", "天津", "39.3434", "117.3616"},
};

std::string UrlDecode(const char* in) {
    std::string out;
    for (size_t i = 0; in[i]; ++i) {
        if (in[i] == '%' && in[i + 1] && in[i + 2]) {
            out += static_cast<char>(strtol(std::string(in + i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}
}  // namespace

void weather_api_start(WeatherCallback callback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_callback = std::move(callback);
    g_started = true;
}

bool weather_api_fetch_now() {
    WeatherCallback cb;
    WeatherData data;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_started) return false;
        data = sim::MakeWeather(g_config.cities[g_config.selected].name);
        g_last = data;
        g_has_last = true;
        cb = g_callback;
    }
    if (cb) cb(data);
    return true;
}

WeatherConfig weather_api_get_config() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_config;
}

bool weather_api_set_config(const WeatherConfig& config) {
    if (!IsValidWeatherConfig(config)) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_config = config;
    }
    weather_api_fetch_now();
    return true;
}

bool weather_api_select_city(int index) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (index < 0 || index >= static_cast<int>(g_config.cities.size())) return false;
        g_config.selected = index;
    }
    return weather_api_fetch_now();
}

bool weather_api_search_city(const char* url_encoded_query, std::vector<WeatherCity>* out) {
    if (!url_encoded_query || !out) return false;
    const std::string q = UrlDecode(url_encoded_query);
    out->clear();
    for (const auto& c : kCities) {
        if (q.empty() || strstr(c.name, q.c_str()) != nullptr) out->push_back({c.name, c.admin, c.lat, c.lon});
    }
    return true;
}

bool weather_api_get_last_data(WeatherData* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_has_last) return false;
    *out = g_last;
    return true;
}
