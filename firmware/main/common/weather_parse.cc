/**
 * @file weather_parse.cc
 * @brief Open-Meteo JSON parsing and WMO code mapping. See weather_parse.h.
 */

#include "weather_parse.h"

#include <cJSON.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
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

static bool IsValidCoordinate(const std::string& v, double limit) {
    if (v.empty() || v.size() > 12) return false;
    char* end = nullptr;
    const double d = strtod(v.c_str(), &end);
    return end && *end == '\0' && d >= -limit && d <= limit;
}

bool IsValidWeatherConfig(const WeatherConfig& c) {
    if (c.cities.empty() || c.cities.size() > kMaxWeatherCities) return false;
    if (c.selected < 0 || c.selected >= static_cast<int>(c.cities.size())) return false;
    if (c.interval_min != 5 && c.interval_min != 10 && c.interval_min != 30) return false;
    for (const auto& city : c.cities) {
        if (city.name.empty() || city.name.size() > 48 || city.admin.size() > 48) return false;
        if (!IsValidCoordinate(city.lat, 90) || !IsValidCoordinate(city.lon, 180)) return false;
    }
    return true;
}

const char* WmoWeatherText(int code) {
    switch (code) {
        case 0: return "晴";
        case 1: return "晴间多云";
        case 2: return "多云";
        case 3: return "阴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: return "毛毛雨";
        case 56: case 57: return "冻雨";
        case 61: return "小雨";
        case 63: return "中雨";
        case 65: return "大雨";
        case 66: case 67: return "冻雨";
        case 71: return "小雪";
        case 73: return "中雪";
        case 75: return "大雪";
        case 77: return "雪粒";
        case 80: return "阵雨";
        case 81: return "中阵雨";
        case 82: return "暴雨";
        case 85: return "阵雪";
        case 86: return "大阵雪";
        case 95: return "雷雨";
        case 96: case 99: return "雷雨冰雹";
        default: return "未知";
    }
}

const char* WmoIconCode(int code) {
    switch (code) {
        case 0: return "100";
        case 1: case 2: return "101";
        case 3: return "104";
        case 45: case 48: return "501";
        case 71: case 73: case 75: case 77: case 85: case 86: return "400";
        case 95: case 96: case 99: return "302";
        default: return (code >= 51 && code <= 82) ? "305" : "100";
    }
}

std::string WindDirectionText(double deg) {
    static const char* kNames[] = {"北风", "东北风", "东风", "东南风", "南风", "西南风", "西风", "西北风"};
    deg = std::fmod(deg, 360.0);
    if (deg < 0) deg += 360.0;
    return kNames[static_cast<int>((deg + 22.5) / 45.0) % 8];
}

int WindBeaufort(double kmh) {
    static const double kLimits[] = {2, 6, 12, 20, 29, 39, 50, 62, 75, 89, 103, 118};
    for (int i = 0; i < 12; ++i) {
        if (kmh < kLimits[i]) return i;
    }
    return 12;
}

const char* AqiCategoryText(int aqi) {
    if (aqi <= 50) return "优";
    if (aqi <= 100) return "良";
    if (aqi <= 150) return "轻度污染";
    if (aqi <= 200) return "中度污染";
    if (aqi <= 300) return "重度污染";
    return "严重污染";
}

const char* UvLevelText(double uv) {
    if (uv < 3) return "弱";
    if (uv < 6) return "中等";
    if (uv < 8) return "强";
    if (uv < 11) return "很强";
    return "极强";
}

int WeekdayFromDate(const char* s) {
    int y = 0, m = 0, d = 0;
    if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

namespace {

double NumberAt(cJSON* arr, int index, double fallback = 0) {
    cJSON* item = cJSON_GetArrayItem(arr, index);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

const char* StringAt(cJSON* arr, int index) {
    cJSON* item = cJSON_GetArrayItem(arr, index);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

int RoundInt(double v) { return static_cast<int>(std::lround(v)); }

bool ParseDaily(cJSON* daily, WeatherData* out) {
    cJSON* times = cJSON_GetObjectItem(daily, "time");
    cJSON* codes = cJSON_GetObjectItem(daily, "weather_code");
    cJSON* tmax = cJSON_GetObjectItem(daily, "temperature_2m_max");
    cJSON* tmin = cJSON_GetObjectItem(daily, "temperature_2m_min");
    if (!cJSON_IsArray(times) || !cJSON_IsArray(codes) || !cJSON_IsArray(tmax) || !cJSON_IsArray(tmin)) {
        return false;
    }
    static const char* kWeekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    const int n = std::min(7, cJSON_GetArraySize(times));
    out->forecast.clear();
    for (int i = 0; i < n; ++i) {
        WeatherForecastDay day;
        const int wd = WeekdayFromDate(StringAt(times, i));
        day.label = i == 0 ? "今天" : i == 1 ? "明天" : (wd >= 0 ? kWeekdays[wd] : "--");
        const int code = RoundInt(NumberAt(codes, i, -1));
        day.weather_text = WmoWeatherText(code);
        day.icon_code = WmoIconCode(code);
        day.temp_max = RoundInt(NumberAt(tmax, i));
        day.temp_min = RoundInt(NumberAt(tmin, i));
        out->forecast.push_back(day);
    }
    cJSON* uv = cJSON_GetObjectItem(daily, "uv_index_max");
    if (cJSON_IsArray(uv) && cJSON_GetArraySize(uv) > 0) {
        out->uv_text = UvLevelText(NumberAt(uv, 0));
    }
    return !out->forecast.empty();
}

void ParseHourly(cJSON* hourly, WeatherData* out) {
    out->hourly.clear();
    cJSON* times = cJSON_GetObjectItem(hourly, "time");
    cJSON* temps = cJSON_GetObjectItem(hourly, "temperature_2m");
    cJSON* codes = cJSON_GetObjectItem(hourly, "weather_code");
    if (!cJSON_IsArray(times) || !cJSON_IsArray(temps) || !cJSON_IsArray(codes)) return;
    const int n = cJSON_GetArraySize(times);
    for (int i = 0; i < n && out->hourly.size() < 6; i += 3) {
        const char* t = StringAt(times, i);  // "2026-10-16T14:00"
        const char* tpos = t ? strchr(t, 'T') : nullptr;
        WeatherHourlyEntry p;
        p.label = out->hourly.empty() ? "现在" : (tpos ? std::to_string(atoi(tpos + 1)) + "时" : "--");
        const int code = RoundInt(NumberAt(codes, i, -1));
        p.icon_code = WmoIconCode(code);
        p.weather_text = WmoWeatherText(code);
        p.temp = RoundInt(NumberAt(temps, i));
        out->hourly.push_back(p);
    }
}

}  // namespace

bool ParseOpenMeteoForecast(const char* json, WeatherData* out) {
    if (!json || !out) return false;
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;

    cJSON* current = cJSON_GetObjectItem(root, "current");
    cJSON* daily = cJSON_GetObjectItem(root, "daily");
    if (!cJSON_IsObject(current) || !cJSON_IsObject(daily)) {
        cJSON_Delete(root);
        return false;
    }

    auto number = [current](const char* key, double* v) {
        cJSON* item = cJSON_GetObjectItem(current, key);
        if (!cJSON_IsNumber(item)) return false;
        *v = item->valuedouble;
        return true;
    };
    double v = 0;
    if (number("temperature_2m", &v)) {
        out->temp_int = RoundInt(v);
        out->temp = std::to_string(out->temp_int);
    }
    if (number("apparent_temperature", &v)) out->feels_like = std::to_string(RoundInt(v));
    if (number("relative_humidity_2m", &v)) out->humidity = std::to_string(RoundInt(v));
    if (number("pressure_msl", &v)) out->pressure = std::to_string(RoundInt(v));
    if (number("weather_code", &v)) {
        out->weather_text = WmoWeatherText(RoundInt(v));
        out->weather_icon = WmoIconCode(RoundInt(v));
    }
    if (number("wind_direction_10m", &v)) out->wind_dir = WindDirectionText(v);
    if (number("wind_speed_10m", &v)) out->wind_scale = std::to_string(WindBeaufort(v));
    cJSON* time_item = cJSON_GetObjectItem(current, "time");  // "2026-10-16T14:30"
    if (cJSON_IsString(time_item) && time_item->valuestring) {
        const char* tpos = strchr(time_item->valuestring, 'T');
        if (tpos && strlen(tpos + 1) >= 5) out->update_time.assign(tpos + 1, 5);
    }

    const bool ok = ParseDaily(daily, out);
    cJSON* hourly = cJSON_GetObjectItem(root, "hourly");
    if (cJSON_IsObject(hourly)) ParseHourly(hourly, out);
    cJSON_Delete(root);
    return ok && !out->temp.empty();
}

bool ParseOpenMeteoAir(const char* json, WeatherData* out) {
    if (!json || !out) return false;
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    cJSON* current = cJSON_GetObjectItem(root, "current");
    cJSON* aqi = cJSON_IsObject(current) ? cJSON_GetObjectItem(current, "us_aqi") : nullptr;
    const bool ok = cJSON_IsNumber(aqi);
    if (ok) {
        out->air_aqi = RoundInt(aqi->valuedouble);
        out->air_quality = AqiCategoryText(out->air_aqi);
    }
    cJSON_Delete(root);
    return ok;
}

bool ParseGeocodeResults(const char* json, std::vector<WeatherCity>* out) {
    if (!json || !out) return false;
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    out->clear();
    cJSON* results = cJSON_GetObjectItem(root, "results");  // absent when nothing matched
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, results) {
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* lat = cJSON_GetObjectItem(item, "latitude");
        cJSON* lon = cJSON_GetObjectItem(item, "longitude");
        if (!cJSON_IsString(name) || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) continue;
        WeatherCity city;
        city.name = name->valuestring;
        cJSON* admin = cJSON_GetObjectItem(item, "admin1");
        if (cJSON_IsString(admin)) city.admin = admin->valuestring;
        cJSON* country = cJSON_GetObjectItem(item, "country");
        if (cJSON_IsString(country) && strcmp(country->valuestring, "中国") != 0 &&
            strcmp(country->valuestring, "China") != 0) {
            city.admin = city.admin.empty() ? country->valuestring
                                            : std::string(country->valuestring) + " " + city.admin;
        }
        char buf[24];
        snprintf(buf, sizeof(buf), "%.4f", lat->valuedouble);
        city.lat = buf;
        snprintf(buf, sizeof(buf), "%.4f", lon->valuedouble);
        city.lon = buf;
        out->push_back(std::move(city));
    }
    cJSON_Delete(root);
    return true;
}
