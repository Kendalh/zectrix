// Unit test for common/chinese_calendar.cc: lunar conversion and festival dates.
// Set LUNAR_DAYS_CSV to a file made by `sim/tools/gen_lunar_table.py PATH --dump-days` to also compare every day
// of 2010-2050 with the reference implementation (the table in lunar_table.inc was generated from it).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/chinese_calendar.h"

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)

namespace {

std::string Festivals(int y, int m, int d) {
    const char* names[cncal::kMaxFestivalsPerDay];
    const int n = cncal::FestivalsOn(y, m, d, names, cncal::kMaxFestivalsPerDay);
    std::string out;
    for (int i = 0; i < n; ++i) out += (i ? "|" : "") + std::string(names[i]);
    return out;
}

void TestKnownDates() {
    CHECK(Festivals(2026, 2, 16) == "除夕");
    CHECK(Festivals(2026, 2, 17) == "春节");
    CHECK(Festivals(2026, 2, 10) == "小年");
    CHECK(Festivals(2026, 1, 26) == "腊八");
    CHECK(Festivals(2026, 3, 3) == "元宵");
    CHECK(Festivals(2026, 4, 5) == "清明");
    CHECK(Festivals(2026, 6, 19) == "端午");
    CHECK(Festivals(2026, 8, 19) == "七夕");
    CHECK(Festivals(2026, 9, 25) == "中秋");
    CHECK(Festivals(2026, 10, 18) == "重阳");
    CHECK(Festivals(2026, 12, 22) == "冬至");
    CHECK(Festivals(2026, 1, 1) == "元旦");
    CHECK(Festivals(2026, 10, 1) == "国庆节");
    CHECK(Festivals(2026, 5, 1) == "劳动节");
    CHECK(Festivals(2026, 9, 10) == "教师节");
    CHECK(Festivals(2025, 1, 28) == "除夕");  // 2025: 腊月 has 29 days
    CHECK(Festivals(2025, 1, 29) == "春节");
    CHECK(Festivals(2020, 10, 1) == "中秋|国庆节");  // two on one day: traditional first
    CHECK(Festivals(2026, 7, 14).empty());
    CHECK(Festivals(2024, 4, 4) == "清明");          // leap year: the formula needs Y/4
    CHECK(Festivals(2021, 12, 21) == "冬至");        // the formula's known exception
    CHECK(std::string(cncal::MainFestival(2020, 10, 1)) == "中秋");
    CHECK(cncal::MainFestival(2026, 7, 14) == nullptr);
}

void TestLunar() {
    cncal::LunarDate l;
    CHECK(cncal::ToLunar(2026, 2, 17, &l) && l.year == 2026 && l.month == 1 && l.day == 1 && !l.leap);
    CHECK(cncal::ToLunar(2026, 2, 16, &l) && l.year == 2025 && l.month == 12 && l.day == 29 && l.last_month &&
          l.month_days == 29);
    CHECK(cncal::ToLunar(2010, 1, 1, &l) && l.year == 2009 && l.month == 11 && l.day == 17);  // before the first CNY
    CHECK(cncal::ToLunar(2025, 7, 25, &l) && l.month == 6 && l.day == 1 && l.leap);            // 2025 has a leap 6th month
    CHECK(cncal::ToLunar(2025, 7, 24, &l) && l.month == 6 && l.day == 30 && !l.leap);
    CHECK(cncal::ToLunar(2050, 12, 31, &l) && l.year == 2050);
    CHECK(!cncal::ToLunar(2009, 12, 31, &l));
    CHECK(!cncal::ToLunar(2051, 1, 1, &l));
    CHECK(Festivals(2009, 10, 1).empty() && Festivals(2051, 10, 1).empty());  // outside the supported years
    CHECK(!cncal::ToLunar(2026, 2, 30, &l));
    CHECK(Festivals(2025, 8, 29) == "七夕");  // 2025: the leap 6th month pushes 七夕 to the end of August
}

void TestAgainstReference() {
    const char* path = getenv("LUNAR_DAYS_CSV");
    if (!path) {
        printf("LUNAR_DAYS_CSV not set: skipped the day-by-day reference check\n");
        return;
    }
    FILE* f = fopen(path, "r");
    CHECK(f != nullptr);
    if (!f) return;
    char line[128];
    int days = 0, bad = 0;
    while (fgets(line, sizeof(line), f)) {
        int y, m, d, ly, lm, ld, leap;
        if (sscanf(line, "%d-%d-%d,%d,%d,%d,%d", &y, &m, &d, &ly, &lm, &ld, &leap) != 7) continue;
        cncal::LunarDate l;
        ++days;
        if (!cncal::ToLunar(y, m, d, &l) || l.year != ly || l.month != lm || l.day != ld || l.leap != (leap != 0)) ++bad;
    }
    fclose(f);
    CHECK(days == 14975 && bad == 0);
    printf("reference check: %d days, %d mismatches\n", days, bad);
}

}  // namespace

int main() {
    TestKnownDates();
    TestLunar();
    TestAgainstReference();
    if (g_failed) {
        fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("test_chinese_calendar: OK\n");
    return 0;
}
