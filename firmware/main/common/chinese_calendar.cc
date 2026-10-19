#include "common/chinese_calendar.h"

#include <cstdint>

#include "common/reminders.h"

namespace cncal {

namespace {

constexpr int kTableFirstYear = 2009;
constexpr int kTableLastYear = 2050;

struct LunarYearInfo {
    uint16_t month_bits;  // bit i set: month i (calendar order, leap month after its regular month) has 30 days
    uint8_t leap_month;   // 0 = none
    uint8_t cny_month;    // Spring Festival, Gregorian
    uint8_t cny_day;
};

const LunarYearInfo kLunarYears[] = {
#include "common/lunar_table.inc"
};
static_assert(sizeof(kLunarYears) / sizeof(kLunarYears[0]) == kTableLastYear - kTableFirstYear + 1,
              "lunar_table.inc must cover 2009..2050");

// Days since 1970-01-01 (proleptic Gregorian).
int DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Solar terms after February: floor(Y*0.2422 + C) - floor(Y/4), Y = last two digits (21st-century constants C).
int QingmingDay(int year) {
    const int y = year % 100;
    return static_cast<int>(y * 0.2422 + 4.81) - y / 4;
}

int DongzhiDay(int year) {
    const int y = year % 100;
    int day = static_cast<int>(y * 0.2422 + 21.94) - y / 4;
    if (year == 2021) --day;  // known exception of the formula
    return day;
}

struct SolarFestival {
    int month, day;
    const char* name;
};

const SolarFestival kSolarFestivals[] = {
    {1, 1, "元旦"},   {3, 8, "妇女节"},  {3, 12, "植树节"}, {5, 1, "劳动节"}, {5, 4, "青年节"},
    {6, 1, "儿童节"}, {7, 1, "建党节"},  {8, 1, "建军节"},  {9, 10, "教师节"}, {10, 1, "国庆节"},
};

}  // namespace

bool ToLunar(int year, int month, int day, LunarDate* out) {
    if (year < kMinYear || year > kMaxYear || !reminders::IsValidDate(year, month, day)) {
        return false;
    }
    const int today = DaysFromCivil(year, month, day);
    int lunar_year = year;
    const LunarYearInfo* info = &kLunarYears[lunar_year - kTableFirstYear];
    if (today < DaysFromCivil(year, info->cny_month, info->cny_day)) {
        --lunar_year;  // before this year's Spring Festival: still in the previous lunar year
        info = &kLunarYears[lunar_year - kTableFirstYear];
    }
    int left = today - DaysFromCivil(lunar_year, info->cny_month, info->cny_day);

    const int count = info->leap_month ? 13 : 12;
    for (int i = 0; i < count; ++i) {
        const int length = (info->month_bits >> i) & 1 ? 30 : 29;
        if (left < length) {
            const bool leap = info->leap_month && i == info->leap_month;
            const int number = (!info->leap_month || i < info->leap_month) ? i + 1 : (leap ? info->leap_month : i);
            *out = {lunar_year, number, left + 1, leap, length, i == count - 1};
            return true;
        }
        left -= length;
    }
    return false;  // table inconsistent
}

int FestivalsOn(int year, int month, int day, const char** names, int max) {
    int n = 0;
    auto add = [&](const char* name) {
        if (n < max) names[n++] = name;
    };

    LunarDate lunar;
    if (!ToLunar(year, month, day, &lunar)) return 0;  // outside the supported years (or invalid)
    if (lunar.last_month && lunar.day == lunar.month_days) add("除夕");
    if (!lunar.leap) {
        struct {
            int month, day;
            const char* name;
        } static const kLunarFestivals[] = {
            {1, 1, "春节"}, {1, 15, "元宵"}, {5, 5, "端午"}, {7, 7, "七夕"},
            {8, 15, "中秋"}, {9, 9, "重阳"}, {12, 8, "腊八"}, {12, 23, "小年"},
        };
        for (const auto& f : kLunarFestivals) {
            if (lunar.month == f.month && lunar.day == f.day) add(f.name);
        }
    }
    if (month == 4 && day == QingmingDay(year)) add("清明");
    if (month == 12 && day == DongzhiDay(year)) add("冬至");
    for (const auto& f : kSolarFestivals) {
        if (month == f.month && day == f.day) add(f.name);
    }
    return n;
}

const char* MainFestival(int year, int month, int day) {
    const char* name = nullptr;
    return FestivalsOn(year, month, day, &name, 1) ? name : nullptr;
}

}  // namespace cncal
