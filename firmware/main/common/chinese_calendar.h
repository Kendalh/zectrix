/**
 * @file chinese_calendar.h
 * @brief Chinese festivals for 2010-2050: Gregorian ones (元旦, 国庆节...), lunar ones (春节, 中秋...) and the
 *        solar terms 清明 and 冬至. Hardware-free; shared by the device calendar page and the web page.
 *
 * The lunar conversion uses a table generated from a reference implementation (lunar_table.inc, see
 * sim/tools/gen_lunar_table.py) and is checked against it day by day. Not covered: the State Council's
 * statutory holiday / make-up workday schedule (it changes every year and needs a data source).
 */

#ifndef COMMON_CHINESE_CALENDAR_H
#define COMMON_CHINESE_CALENDAR_H

namespace cncal {

struct LunarDate {
    int year;         ///< lunar year number (the Gregorian year it starts in)
    int month;        ///< 1-12
    int day;          ///< 1-30
    bool leap;        ///< leap month (闰月)
    int month_days;   ///< 29 or 30
    bool last_month;  ///< last month of the lunar year (除夕 is its last day)
};

constexpr int kMinYear = 2010;
constexpr int kMaxYear = 2050;

/** @return false outside 2010-01-01..2050-12-31 or for an invalid date. */
bool ToLunar(int year, int month, int day, LunarDate* out);

constexpr int kMaxFestivalsPerDay = 4;

/**
 * Festivals on a date, most important first (traditional before national). 0 outside kMinYear..kMaxYear. Returns how many were written
 * to `names` (at most `max`); the strings are static.
 */
int FestivalsOn(int year, int month, int day, const char** names, int max);

/** The first festival of the day, or nullptr. */
const char* MainFestival(int year, int month, int day);

}  // namespace cncal

#endif  // COMMON_CHINESE_CALENDAR_H
