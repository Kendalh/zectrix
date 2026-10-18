/**
 * @file weather_parse.h
 * @brief Pure Open-Meteo response parsing and WMO code mapping (no network, no ESP-IDF).
 *
 * Shared by the firmware (weather_api.cc) and the PC simulator. Icon codes use the QWeather
 * numbering the renderers already understand: 100 sunny, 101 partly cloudy, 104 overcast,
 * 305 rain, 302 thunderstorm, 400 snow, 501 fog.
 */

#ifndef WEATHER_PARSE_H
#define WEATHER_PARSE_H

#include "weather_api.h"

#include <string>
#include <vector>

/** True if 1..5 cities with valid coordinates, a valid selection and interval (5/10/30 min). */
bool IsValidWeatherConfig(const WeatherConfig& config);

/** WMO weather interpretation code -> Chinese text (晴, 多云, 小雨 ...). */
const char* WmoWeatherText(int wmo_code);

/** WMO weather interpretation code -> icon code string ("100", "305" ...). */
const char* WmoIconCode(int wmo_code);

/** Wind direction in degrees -> "东南风" style text. */
std::string WindDirectionText(double degrees);

/** Wind speed in km/h -> Beaufort scale 0..12. */
int WindBeaufort(double kmh);

/** US AQI -> Chinese quality category (优/良/轻度污染 ...). */
const char* AqiCategoryText(int us_aqi);

/** UV index -> 弱/中等/强/很强/极强. */
const char* UvLevelText(double uv_index);

/** "2026-10-16" -> weekday 0=Sunday..6=Saturday, or -1 if malformed. */
int WeekdayFromDate(const char* yyyy_mm_dd);

/**
 * Parse /v1/forecast (current + daily + hourly). Fills everything in `out` except city, air quality.
 * @return false if the JSON is malformed or has no current/daily block.
 */
bool ParseOpenMeteoForecast(const char* json, WeatherData* out);

/** Parse the air-quality API (current.us_aqi) into air_aqi / air_quality. */
bool ParseOpenMeteoAir(const char* json, WeatherData* out);

/** Parse the geocoding search API (results[]). Empty result list is a success. */
bool ParseGeocodeResults(const char* json, std::vector<WeatherCity>* out);

#endif  // WEATHER_PARSE_H
