/**
 * @file weather_api.h
 * @brief Weather data model and client (Open-Meteo, no API key)
 *
 * Current conditions + 7-day forecast + air quality over HTTPS, refreshed on a user-chosen
 * interval. Pure parsing lives in weather_parse.h so the simulator can share it.
 */

#ifndef WEATHER_API_H
#define WEATHER_API_H

#include <stdint.h>
#include <stdbool.h>
#include <string>
#include <functional>
#include <vector>

// ============================================================
// Weather data model
// ============================================================

/**
 * @brief One day of the daily forecast (up to 7 days)
 */
struct WeatherForecastDay {
    std::string label;        // 今天 / 明天 / 周X
    std::string weather_text; // 晴 / 多云 / 小雨
    std::string icon_code;    // QWeather-style icon code string (100 sunny, 101 cloudy, ...)
    int32_t temp_min = 0;
    int32_t temp_max = 0;
};

struct WeatherHourlyEntry {
    std::string label;        // "14时"
    std::string icon_code;
    std::string weather_text;
    int32_t temp = 0;
};

struct WeatherData {
    std::string city;         // City name in Chinese
    std::string temp;         // Current temperature (e.g., "25")
    std::string feels_like;   // Feels like temperature (e.g., "27")
    std::string weather_icon; // Icon code for current weather (e.g., "100")
    std::string weather_text; // Weather condition (e.g., "晴", "多云", "小雨")
    std::string wind_dir;     // Wind direction (e.g., "东南风")
    std::string wind_scale;   // Wind scale, Beaufort (e.g., "3")
    std::string humidity;     // Humidity percentage (e.g., "45")
    std::string pressure;     // Sea-level pressure in hPa (e.g., "1012")
    std::string uv_text;      // Today's peak UV level (弱/中等/强/很强/极强)
    std::string update_time;  // Observation time (e.g., "14:30")
    std::string air_quality;  // Air quality text (e.g., "优")
    int32_t air_aqi = -1;     // AQI number (US AQI), -1 when unknown
    int32_t temp_int = 0;     // Numeric temperature for icon selection
    std::vector<WeatherForecastDay> forecast;  // up to 7 days, first is today
    std::vector<WeatherHourlyEntry> hourly;      // every 3 hours, starting now
};

/** A city returned by the geocoding search. */
struct WeatherCity {
    std::string name;     // 杭州市
    std::string admin;    // 浙江 (province / region, may be empty)
    std::string lat;      // decimal degrees, kept as text (NVS has no float)
    std::string lon;
};

constexpr size_t kMaxWeatherCities = 5;

/** User-selected cities (up to 5), the one shown on the device, and the refresh frequency. Persisted. */
struct WeatherConfig {
    std::vector<WeatherCity> cities = {{"杭州", "浙江", "30.2936", "120.1614"}};
    int selected = 0;         // index into cities; the device page shows this one
    int interval_min = 30;    // 5, 10 or 30
};

/**
 * @brief Weather icon codes for 1bpp rendering
 * Maps weather condition text to icon character codes.
 */
enum class WeatherIcon {
    Sunny,       // 晴
    Cloudy,      // 多云
    Overcast,    // 阴
    Rain,        // 雨 (any rain type)
    Snow,        // 雪
    Fog,         // 雾
    Unknown,     // Fallback
};

/**
 * @brief Map weather condition text to icon type
 */
WeatherIcon ParseWeatherIcon(const char* weather_text);

// ============================================================
// API interface (data source: Open-Meteo, free, no API key)
//   https://open-meteo.com/en/docs
//   https://open-meteo.com/en/docs/geocoding-api
//   https://open-meteo.com/en/docs/air-quality-api
// ============================================================

/**
 * @brief Callback type for weather data delivery (called on the weather task)
 */
using WeatherCallback = std::function<void(const WeatherData&)>;

/**
 * @brief Start the weather task (idempotent).
 *
 * Loads the saved WeatherConfig and creates the task, which then stays idle: **nothing is fetched at boot or when
 * Wi-Fi connects, and there is no timer**. Weather is fetched only on request: weather_api_refresh_if_stale() (the
 * Weather page was entered), weather_api_fetch_now() (refresh key), weather_api_select_city(), weather_api_set_config().
 * A failed fetch (no network) is not retried by itself; the next request tries again.
 */
void weather_api_start(WeatherCallback callback);

/**
 * The Weather page was entered: fetch the selected city only if the cached data is older than interval_min (the data's
 * validity); a fresh cache means the page already shows it and no request is made. @return false if the task is not running.
 */
bool weather_api_refresh_if_stale();

/** Request an immediate refresh, ignoring the cache. @return false if the task is not running. */
bool weather_api_fetch_now();

/** Current configuration (persisted). */
WeatherConfig weather_api_get_config();

/** Validate, persist and apply a new configuration, then refresh now. @return false if invalid. */
bool weather_api_set_config(const WeatherConfig& config);

/**
 * Switch the shown city (device UP/DOWN). Persisted; non-blocking: the result (cached data when
 * fresh, otherwise a new fetch) arrives through the callback. @return false if index is invalid.
 */
bool weather_api_select_city(int index);

/** Search cities by name (Chinese or English). Blocking HTTP call. @return false on network/parse error. */
bool weather_api_search_city(const char* url_encoded_query, std::vector<WeatherCity>* out);

/** Copy of the last fetched data. @return false if nothing has been fetched yet. */
bool weather_api_get_last_data(WeatherData* out);

#endif  // WEATHER_API_H
