/**
 * @file weather_api.cc
 * @brief Open-Meteo weather client (HTTPS, no API key). See weather_api.h.
 *
 * A dedicated task does the (slow, TLS) fetching so neither the esp_timer task nor the HTTP
 * server task needs a big stack for it. The task is idle and only works when
 * woken by weather_api_refresh_if_stale() / weather_api_fetch_now() / weather_api_set_config().
 *
 *   https://api.open-meteo.com/v1/forecast          current + 7-day daily + hourly
 *   https://air-quality-api.open-meteo.com/v1/air-quality   US AQI
 *   https://geocoding-api.open-meteo.com/v1/search  city search
 */

#include "weather_api.h"
#include "weather_parse.h"
#include "settings.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

static const char* kTag = "WeatherApi";

namespace {

constexpr char kNamespace[] = "weather";
constexpr size_t kResponseMax = 12 * 1024;

std::mutex s_mutex;  // guards s_config, s_last, s_has_last, s_cache
WeatherConfig s_config;
WeatherData s_last;
bool s_has_last = false;
// Per-city cache (index = position in s_config.cities) so switching back and forth is instant.
struct CacheEntry {
    WeatherData data;
    bool valid = false;
    int64_t fetched_ms = 0;
};
CacheEntry s_cache[kMaxWeatherCities];
std::atomic<bool> s_force{false};    // next wake-up must fetch even if the cache is fresh
std::atomic<bool> s_deliver{false};  // next wake-up delivers the fresh cache (or fetches): city switched
std::atomic<bool> s_entry{false};    // next wake-up fetches only if the cache is stale: Weather page entered
WeatherCallback s_callback;
TaskHandle_t s_task = nullptr;
std::mutex s_http_mutex;  // one HTTP request at a time (shared response buffer)

std::string Key(char field, int i) { return std::string("c") + std::to_string(i) + field; }

void LoadConfig() {
    Settings nvs(kNamespace, false);
    WeatherConfig c;
    const int count = nvs.GetInt("count", 0);
    std::vector<WeatherCity> cities;
    for (int i = 0; i < count && i < static_cast<int>(kMaxWeatherCities); ++i) {
        cities.push_back({nvs.GetString(Key('n', i), ""), nvs.GetString(Key('a', i), ""),
                          nvs.GetString(Key('y', i), ""), nvs.GetString(Key('x', i), "")});
    }
    if (cities.empty()) {  // single-city config written by the previous firmware version
        cities.push_back({nvs.GetString("city", ""), "", nvs.GetString("lat", ""), nvs.GetString("lon", "")});
    }
    WeatherConfig saved;
    saved.cities = cities;
    saved.selected = nvs.GetInt("sel", 0);
    saved.interval_min = nvs.GetInt("interval", c.interval_min);
    if (IsValidWeatherConfig(saved)) c = saved;  // otherwise nothing (valid) saved yet: use defaults
    std::lock_guard<std::mutex> lock(s_mutex);
    s_config = c;
}

void SaveConfig(const WeatherConfig& c) {
    Settings nvs(kNamespace, true);
    nvs.SetInt("count", static_cast<int>(c.cities.size()));
    for (size_t i = 0; i < c.cities.size(); ++i) {
        nvs.SetString(Key('n', i), c.cities[i].name);
        nvs.SetString(Key('a', i), c.cities[i].admin);
        nvs.SetString(Key('y', i), c.cities[i].lat);
        nvs.SetString(Key('x', i), c.cities[i].lon);
    }
    nvs.SetInt("sel", c.selected);
    nvs.SetInt("interval", c.interval_min);
}

// ---- HTTP ----------------------------------------------------------------

struct Response {
    std::string body;
    bool overflow = false;
};

esp_err_t HttpEvent(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        auto* r = static_cast<Response*>(evt->user_data);
        if (r->body.size() + evt->data_len > kResponseMax) {
            r->overflow = true;
        } else {
            r->body.append(static_cast<const char*>(evt->data), evt->data_len);
        }
    }
    return ESP_OK;
}

bool HttpGet(const std::string& url, std::string* body) {
    std::lock_guard<std::mutex> lock(s_http_mutex);
    Response response;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.event_handler = HttpEvent;
    config.user_data = &response;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "HTTP failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200 || response.overflow) {
        ESP_LOGW(kTag, "HTTP status %d overflow=%d", status, response.overflow ? 1 : 0);
        return false;
    }
    *body = std::move(response.body);
    return true;
}

// ---- fetch ---------------------------------------------------------------

bool FetchOnce() {
    WeatherCity city;
    int index = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        index = s_config.selected;
        city = s_config.cities[index];
    }
    WeatherData data;
    data.city = city.name;
    std::string body;

    std::string url = "https://api.open-meteo.com/v1/forecast?latitude=" + city.lat +
        "&longitude=" + city.lon +
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,"
        "pressure_msl,wind_speed_10m,wind_direction_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,uv_index_max"
        "&hourly=temperature_2m,weather_code&forecast_hours=16"
        "&timezone=Asia%2FShanghai&forecast_days=7";
    if (!HttpGet(url, &body) || !ParseOpenMeteoForecast(body.c_str(), &data)) {
        ESP_LOGW(kTag, "Forecast fetch/parse failed");
        return false;
    }

    url = "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=" + city.lat +
          "&longitude=" + city.lon + "&current=us_aqi&timezone=Asia%2FShanghai";
    if (!HttpGet(url, &body) || !ParseOpenMeteoAir(body.c_str(), &data)) {
        ESP_LOGW(kTag, "Air quality unavailable (keeping weather)");  // not fatal; common outside cities
    }

    ESP_LOGI(kTag, "Weather %s: %s %s C, %d forecast days, AQI %d", data.city.c_str(),
             data.weather_text.c_str(), data.temp.c_str(), static_cast<int>(data.forecast.size()),
             static_cast<int>(data.air_aqi));
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        // The user may have switched/edited cities while the request was in flight.
        if (index == s_config.selected && index < static_cast<int>(s_config.cities.size()) &&
            city.name == s_config.cities[index].name) {
            s_cache[index] = {data, true, esp_timer_get_time() / 1000};
            s_last = data;
            s_has_last = true;
        } else {
            return true;  // stale result; the task loop fetches the new selection next
        }
    }
    if (s_callback) s_callback(data);
    return true;
}

// The cached data for the selected city if it is still fresh (younger than interval_min). @return true if there is any.
bool FreshCache(WeatherData* out) {
    std::lock_guard<std::mutex> lock(s_mutex);
    const CacheEntry& e = s_cache[s_config.selected];
    const int64_t age_ms = esp_timer_get_time() / 1000 - e.fetched_ms;
    if (!e.valid || age_ms >= static_cast<int64_t>(s_config.interval_min) * 60 * 1000) return false;
    *out = e.data;
    return true;
}

// Deliver the cached data for the selected city if it is still fresh. @return true if delivered.
bool DeliverFreshCache() {
    WeatherData data;
    if (!FreshCache(&data)) return false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_last = data;
        s_has_last = true;
    }
    if (s_callback) s_callback(data);
    return true;
}

// The task is idle until asked: nothing is fetched at boot, when Wi-Fi connects, or on a timer. Weather is only needed
// while the Weather page is open, so the page asks when it is entered (weather_api_refresh_if_stale); the other
// triggers are explicit user actions (refresh key, switching city, saving the web settings).
void WeatherTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const bool forced = s_force.exchange(false);
        const bool deliver = s_deliver.exchange(false);
        const bool entry = s_entry.exchange(false);
        WeatherData unused;
        if (forced) FetchOnce();
        else if (deliver) { if (!DeliverFreshCache()) FetchOnce(); }  // city switch: cached result or a new fetch
        else if (entry) { if (!FreshCache(&unused)) FetchOnce(); }    // page entry: the page already shows fresh data
    }
}

}  // namespace

void weather_api_start(WeatherCallback callback) {
    if (s_task) return;
    LoadConfig();
    s_callback = std::move(callback);
    // TLS handshake needs a generous stack.
    if (xTaskCreate(WeatherTask, "weather", 8192, nullptr, 3, &s_task) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create weather task");
        s_task = nullptr;
    }
}

bool weather_api_fetch_now() {
    if (!s_task) return false;
    s_force = true;
    xTaskNotifyGive(s_task);
    return true;
}

WeatherConfig weather_api_get_config() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_config;
}

bool weather_api_set_config(const WeatherConfig& config) {
    if (!IsValidWeatherConfig(config)) return false;
    SaveConfig(config);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_config = config;
        for (auto& e : s_cache) e.valid = false;  // indexes may now refer to different cities
    }
    ESP_LOGI(kTag, "Config: %d cities, showing #%d, every %d min", static_cast<int>(config.cities.size()),
             config.selected, config.interval_min);
    weather_api_fetch_now();
    return true;
}

bool weather_api_select_city(int index) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (index < 0 || index >= static_cast<int>(s_config.cities.size())) return false;
        if (index == s_config.selected) return true;
        s_config.selected = index;
        WeatherConfig copy = s_config;
        // SaveConfig takes no lock; write the whole config so selection and list stay consistent.
        SaveConfig(copy);
    }
    if (s_task) {
        s_deliver = true;  // cached data is delivered, otherwise fetched
        xTaskNotifyGive(s_task);
    }
    return true;
}

bool weather_api_refresh_if_stale() {
    if (!s_task) return false;
    s_entry = true;
    xTaskNotifyGive(s_task);
    return true;
}

bool weather_api_search_city(const char* url_encoded_query, std::vector<WeatherCity>* out) {
    if (!url_encoded_query || !url_encoded_query[0] || !out) return false;
    const std::string url =
        std::string("https://geocoding-api.open-meteo.com/v1/search?count=8&language=zh&format=json&name=") +
        url_encoded_query;
    std::string body;
    return HttpGet(url, &body) && ParseGeocodeResults(body.c_str(), out);
}

bool weather_api_get_last_data(WeatherData* out) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_has_last) return false;
    *out = s_last;
    return true;
}
