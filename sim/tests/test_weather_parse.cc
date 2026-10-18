// Unit test for common/weather_parse.cc (the real firmware parser) against Open-Meteo-shaped JSON.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../sim_fixtures.h"

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b) CHECK(std::string(a) == std::string(b))

int main() {
    WeatherData d = sim::MakeWeather("杭州");
    CHECK_EQ(d.city, "杭州");
    CHECK_EQ(d.temp, "25");            // 24.6 rounds
    CHECK_EQ(d.feels_like, "27");
    CHECK_EQ(d.humidity, "45");
    CHECK_EQ(d.pressure, "1012");
    CHECK_EQ(d.weather_text, "多云");  // WMO 2
    CHECK_EQ(d.weather_icon, "101");
    CHECK_EQ(d.wind_dir, "东南风");    // 135 deg
    CHECK_EQ(d.wind_scale, "3");       // 14.2 km/h
    CHECK_EQ(d.update_time, "14:30");
    CHECK_EQ(d.uv_text, "中等");       // 5.8
    CHECK(d.air_aqi == 38);
    CHECK_EQ(d.air_quality, "优");

    CHECK(d.forecast.size() == 7);
    CHECK_EQ(d.forecast[0].label, "今天");
    CHECK_EQ(d.forecast[1].label, "明天");
    CHECK_EQ(d.forecast[2].label, "周日");  // 2026-10-18 is a Sunday
    CHECK_EQ(d.forecast[6].label, "周四");
    CHECK_EQ(d.forecast[1].weather_text, "小雨");
    CHECK_EQ(d.forecast[1].icon_code, "305");
    CHECK_EQ(d.forecast[5].weather_text, "雾");
    CHECK_EQ(d.forecast[6].weather_text, "雷雨");
    CHECK(d.forecast[0].temp_max == 27 && d.forecast[0].temp_min == 18);

    CHECK(d.hourly.size() == 6);
    CHECK_EQ(d.hourly[0].label, "现在");
    CHECK_EQ(d.hourly[1].label, "17时");
    CHECK_EQ(d.hourly[5].label, "5时");  // every 3 h: 14,17,20,23,2,5

    // Malformed / incomplete responses must fail cleanly.
    WeatherData bad;
    CHECK(!ParseOpenMeteoForecast("not json", &bad));
    CHECK(!ParseOpenMeteoForecast("{\"current\":{}}", &bad));
    CHECK(!ParseOpenMeteoForecast("{\"reason\":\"x\",\"error\":true}", &bad));
    CHECK(!ParseOpenMeteoAir("{\"current\":{}}", &bad));

    std::vector<WeatherCity> cities;
    CHECK(ParseGeocodeResults(R"({"results":[{"name":"杭州市","latitude":30.29366,"longitude":120.16142,"admin1":"浙江","country":"中国"},
        {"name":"Hangzhou","latitude":1.5,"longitude":2.5,"country":"Nowhere"},{"name":"bad"}]})", &cities));
    CHECK(cities.size() == 2);
    CHECK_EQ(cities[0].name, "杭州市");
    CHECK_EQ(cities[0].admin, "浙江");
    CHECK_EQ(cities[0].lat, "30.2937");
    CHECK_EQ(cities[1].admin, "Nowhere");
    CHECK(ParseGeocodeResults("{\"generationtime_ms\":0.5}", &cities) && cities.empty());  // no match is not an error

    {
        WeatherConfig c;
        CHECK(IsValidWeatherConfig(c));
        c.interval_min = 7;
        CHECK(!IsValidWeatherConfig(c));
        c.interval_min = 10;
        c.cities[0].lat = "999";
        CHECK(!IsValidWeatherConfig(c));
        c.cities[0].lat = "30.1";
        c.selected = 1;  // out of range with one city
        CHECK(!IsValidWeatherConfig(c));
        c.selected = 0;
        for (int i = 0; i < 4; ++i) c.cities.push_back({"c" + std::to_string(i), "", "1", "2"});
        CHECK(c.cities.size() == 5 && IsValidWeatherConfig(c));
        c.cities.push_back({"six", "", "1", "2"});  // more than 5 cities
        CHECK(!IsValidWeatherConfig(c));
        c.cities.pop_back();
        c.cities.clear();
        CHECK(!IsValidWeatherConfig(c));  // at least one city
    }
    CHECK(WeekdayFromDate("2026-10-16") == 5);  // Friday
    CHECK(WeekdayFromDate("garbage") == -1);
    CHECK(WindBeaufort(0) == 0 && WindBeaufort(30) == 5 && WindBeaufort(200) == 12);
    CHECK_EQ(WindDirectionText(0), "北风");
    CHECK_EQ(WindDirectionText(359), "北风");
    CHECK_EQ(AqiCategoryText(120), "轻度污染");
    CHECK_EQ(UvLevelText(11), "极强");

    if (g_failed) return 1;
    printf("weather parse OK\n");
    return 0;
}
