// Unit test for common/reminders.cc (date helpers, Store, file persistence) and the calendar app's
// button logic (CalendarAppRenderer), both compiled unmodified from firmware/main.
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "../sim_fixtures.h"
#include "common/reminders.h"
#include "ui/renderers/rawdraw/calendar_app_renderer.h"

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)

using reminders::SetResult;
using reminders::Store;

namespace {

class MemoryPersistence : public reminders::Persistence {
public:
    bool Read(std::string* out) override {
        if (!has_) return false;
        *out = data_;
        return true;
    }
    bool Write(const std::string& data) override {
        ++writes_;
        if (fail_) return false;
        data_ = data;
        has_ = true;
        return true;
    }
    std::string data_;
    bool has_ = false;
    bool fail_ = false;
    int writes_ = 0;
};

// The store takes ownership, so tests keep a raw pointer to inspect/steer the fake.
MemoryPersistence* Attach(Store* store, const std::string& initial = "", bool has = false) {
    auto p = std::make_unique<MemoryPersistence>();
    p->data_ = initial;
    p->has_ = has;
    auto* raw = p.get();
    store->SetPersistence(std::move(p));
    return raw;
}

void TestDates() {
    CHECK(reminders::DaysInMonth(2024, 2) == 29);
    CHECK(reminders::DaysInMonth(2026, 2) == 28);
    CHECK(reminders::DaysInMonth(2100, 2) == 28);  // not a leap year
    CHECK(reminders::DaysInMonth(2000, 2) == 29);
    CHECK(reminders::DaysInMonth(2026, 13) == 0);
    CHECK(reminders::Weekday(2026, 10, 19) == 1);  // Monday
    CHECK(reminders::Weekday(2026, 10, 1) == 4);   // Thursday
    CHECK(reminders::Weekday(2000, 1, 1) == 6);    // Saturday
    CHECK(reminders::Weekday(2026, 1, 1) == 4);
    CHECK(reminders::IsValidDate(2026, 2, 28) && !reminders::IsValidDate(2026, 2, 29));
    CHECK(!reminders::IsValidDate(1999, 12, 31) && !reminders::IsValidDate(2100, 1, 1));
    CHECK(reminders::DateKey(2026, 3, 7) == "2026-03-07");
    int y, m, d;
    CHECK(reminders::ParseDateKey("2026-10-19", &y, &m, &d) && y == 2026 && m == 10 && d == 19);
    CHECK(!reminders::ParseDateKey("2026-1-9", &y, &m, &d));
    CHECK(!reminders::ParseDateKey("2026-02-30", &y, &m, &d));
    CHECK(!reminders::ParseDateKey("20261019xx", &y, &m, &d));
    CHECK(!reminders::ParseDateKey("2026-10-1a", &y, &m, &d));
}

void TestText() {
    CHECK(reminders::NormalizeText("  开会\n") == "开会");
    CHECK(reminders::NormalizeText("a\tb") == "a b");
    CHECK(reminders::NormalizeText(" \n ").empty());
    CHECK(reminders::IsValidUtf8("开会 abc"));
    CHECK(!reminders::IsValidUtf8("\xff"));
    CHECK(!reminders::IsValidUtf8("\xe5\xbc"));          // truncated
    CHECK(!reminders::IsValidUtf8("\xc0\x80"));          // overlong
    CHECK(!reminders::IsValidUtf8("\xed\xa0\x80"));      // surrogate
}

void TestStore() {
    Store store;
    auto* mem = Attach(&store);
    CHECK(store.Get(2026, 10, 19).empty() && !store.Has(2026, 10, 19));

    int changes = 0;
    store.SetOnChange([&changes]() { ++changes; });
    CHECK(store.Set(2026, 10, 19, {" 开会 ", "", "买牛奶"}) == SetResult::Ok);
    CHECK(changes == 1);
    CHECK((store.Get(2026, 10, 19) == std::vector<std::string>{"开会", "买牛奶"}));
    CHECK(store.Has(2026, 10, 19));
    store.Set(2026, 10, 3, {"国庆"});
    const unsigned mask = store.DaysWithReminders(2026, 10);
    CHECK(mask == ((1u << 3) | (1u << 19)));
    CHECK(store.DaysWithReminders(2026, 11) == 0);

    // Persisted, and a fresh store over the same data sees it.
    Store other;
    Attach(&other, mem->data_, true);
    CHECK((other.Get(2026, 10, 19) == std::vector<std::string>{"开会", "买牛奶"}));
    CHECK(other.All().size() == 2);

    // Deleting a date and no-op deletes.
    const int writes = mem->writes_;
    CHECK(store.Set(2026, 10, 20, {}) == SetResult::Ok);  // nothing stored: no write, no notification
    CHECK(mem->writes_ == writes && changes == 2);
    CHECK(store.Set(2026, 10, 3, {"  "}) == SetResult::Ok);
    CHECK(!store.Has(2026, 10, 3));

    // Validation: rejected without touching anything.
    CHECK(store.Set(2026, 2, 30, {"x"}) == SetResult::BadDate);
    CHECK(store.Set(2026, 10, 19, {std::string(97, 'x')}) == SetResult::TextTooLong);
    CHECK(store.Set(2026, 10, 19, std::vector<std::string>(7, "x")) == SetResult::TooMany);
    CHECK(store.Set(2026, 10, 19, {"\xff"}) == SetResult::BadText);
    CHECK(store.Set(2026, 10, 19, {std::string(96, 'x')}) == SetResult::Ok);  // exactly at the limit
    CHECK(store.Get(2026, 10, 19).at(0).size() == 96);

    // A failed save rolls back (changed, new and deleted dates).
    mem->fail_ = true;
    const int before = changes;
    CHECK(store.Set(2026, 10, 19, {"neu"}) == SetResult::SaveFailed);
    CHECK(store.Get(2026, 10, 19).at(0).size() == 96);
    CHECK(store.Set(2026, 12, 1, {"new"}) == SetResult::SaveFailed && !store.Has(2026, 12, 1));
    CHECK(store.Set(2026, 10, 19, {}) == SetResult::SaveFailed && store.Has(2026, 10, 19));
    CHECK(changes == before);  // no notification for failures
    mem->fail_ = false;

    // Date limit.
    Store full;
    Attach(&full);
    int stored = 0;
    for (int m = 1; m <= 12 && stored < reminders::kMaxDates; ++m) {
        for (int d = 1; d <= reminders::DaysInMonth(2026, m) && stored < reminders::kMaxDates; ++d, ++stored) {
            CHECK(full.Set(2026, m, d, {"x"}) == SetResult::Ok);
        }
    }
    CHECK(stored == reminders::kMaxDates);
    CHECK(full.Set(2027, 1, 1, {"x"}) == SetResult::TooManyDates);
    CHECK(full.Set(2026, 1, 1, {"y"}) == SetResult::Ok);  // editing an existing date is still fine
}

void TestLoadSanitizes() {
    Store store;
    Attach(&store,
           R"({"2026-10-19":["ok","  ",7,"é"],"2026-02-30":["bad date"],"nope":["x"],"2026-10-20":"notalist",)"
           R"("2026-10-21":["1","2","3","4","5","6","7","8"]})",
           true);
    CHECK((store.Get(2026, 10, 19) == std::vector<std::string>{"ok", "\xc3\xa9"}));
    CHECK(store.All().size() == 2);  // 10-19 and 10-21
    CHECK(store.Get(2026, 10, 21).size() == static_cast<size_t>(reminders::kMaxPerDay));

    Store corrupt;
    Attach(&corrupt, "{not json", true);
    CHECK(corrupt.All().empty());
    Store array;
    Attach(&array, "[1,2]", true);
    CHECK(array.All().empty());
}

void TestFilePersistence() {
    const std::string path = "/tmp/test_reminders_file.json";
    remove(path.c_str());
    {
        Store store;
        store.SetPersistence(reminders::NewFilePersistence(path));
        CHECK(store.All().empty());  // missing file is fine
        CHECK(store.Set(2026, 10, 19, {"开会", "quote \" and \\ backslash"}) == SetResult::Ok);
        CHECK(store.Set(2026, 10, 19, {"开会改期"}) == SetResult::Ok);  // overwrite goes through rename
    }
    Store reopened;
    reopened.SetPersistence(reminders::NewFilePersistence(path));
    CHECK((reopened.Get(2026, 10, 19) == std::vector<std::string>{"开会改期"}));
    FILE* tmp = fopen((path + ".tmp").c_str(), "rb");
    CHECK(tmp == nullptr);  // no temp file left behind
    if (tmp) fclose(tmp);
    remove(path.c_str());

    Store unwritable;
    unwritable.SetPersistence(reminders::NewFilePersistence("/nonexistent_dir/reminders.json"));
    CHECK(unwritable.Set(2026, 10, 19, {"x"}) == SetResult::SaveFailed);
}

// ---- calendar app ----

time_t At(int y, int m, int d) {
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    return mktime(&t);
}

rawdraw::ButtonEvent Ev(rawdraw::ButtonEvent::Type type) { return {type}; }

void TestCalendarApp() {
    using rawdraw::ButtonEvent;
    Store store;
    Attach(&store);
    time_t now = At(2026, 10, 19);
    rawdraw::CalendarAppRenderer cal;
    cal.SetStore(&store);
    cal.SetClock([&now]() { return now; });
    cal.Init(400, 300);
    CHECK(cal.ViewYear() == 2026 && cal.ViewMonth() == 10 && !cal.ShowingToday());

    // UP/DOWN walk months, across year boundaries.
    CHECK(cal.HandleInput(Ev(ButtonEvent::kDownClick)) && cal.ViewMonth() == 11);
    cal.HandleInput(Ev(ButtonEvent::kDownClick));
    CHECK(cal.ViewYear() == 2026 && cal.ViewMonth() == 12);
    cal.HandleInput(Ev(ButtonEvent::kDownClick));
    CHECK(cal.ViewYear() == 2027 && cal.ViewMonth() == 1);
    cal.HandleInput(Ev(ButtonEvent::kUpClick));
    CHECK(cal.ViewYear() == 2026 && cal.ViewMonth() == 12);

    // BOOT away from the current month returns to it (and does NOT open the day view).
    CHECK(cal.HandleInput(Ev(ButtonEvent::kBootClick)));
    CHECK(cal.ViewYear() == 2026 && cal.ViewMonth() == 10 && !cal.ShowingToday());
    // BOOT in the current month opens today's reminders; BOOT again goes back.
    CHECK(cal.HandleInput(Ev(ButtonEvent::kBootClick)) && cal.ShowingToday());
    CHECK(cal.HandleInput(Ev(ButtonEvent::kBootClick)) && !cal.ShowingToday());

    // A past/future month is not "current": one BOOT only returns.
    for (int i = 0; i < 12; ++i) cal.HandleInput(Ev(ButtonEvent::kUpClick));
    CHECK(cal.ViewYear() == 2025 && cal.ViewMonth() == 10);
    cal.HandleInput(Ev(ButtonEvent::kBootClick));
    CHECK(cal.ViewYear() == 2026 && !cal.ShowingToday());

    // Unhandled events are not consumed (global handling such as BOOT long happens elsewhere).
    CHECK(!cal.HandleInput(Ev(ButtonEvent::kBootLongPress)));
    CHECK(!cal.HandleInput(Ev(ButtonEvent::kUpLongPress)));

    // Range end: stays at the last supported month.
    Store s2;
    Attach(&s2);
    time_t edge = At(2099, 12, 5);
    rawdraw::CalendarAppRenderer end;
    end.SetStore(&s2);
    end.SetClock([&edge]() { return edge; });
    end.Init(400, 300);
    end.HandleInput(Ev(ButtonEvent::kDownClick));
    CHECK(end.ViewYear() == 2099 && end.ViewMonth() == 12);

    // The month rolls over with the clock: "current month" follows the date.
    now = At(2026, 11, 1);
    cal.HandleInput(Ev(ButtonEvent::kBootClick));  // view was Oct 2026, now is Nov -> go to Nov
    CHECK(cal.ViewMonth() == 11 && !cal.ShowingToday());
    cal.HandleInput(Ev(ButtonEvent::kBootClick));
    CHECK(cal.ShowingToday());

    // Scrolling the day view never goes out of range.
    now = At(2026, 10, 19);
    store.Set(2026, 10, 19, {"a", "b", "c"});
    cal.Init(400, 300);
    cal.HandleInput(Ev(ButtonEvent::kBootClick));
    CHECK(cal.ShowingToday());
    for (int i = 0; i < 10; ++i) CHECK(cal.HandleInput(Ev(ButtonEvent::kDownClick)));
    for (int i = 0; i < 10; ++i) CHECK(cal.HandleInput(Ev(ButtonEvent::kUpClick)));
    CHECK(cal.ShowingToday());

    // Rendering never crashes: 4-, 5- and 6-row months, empty/long/many reminders, both views.
    std::vector<uint8_t> fb(sim::kBpr * sim::kH);
    store.Set(2026, 10, 19, std::vector<std::string>(6, std::string(96, 'x')));
    store.Set(2026, 2, 28, {"边界"});
    for (int month = 1; month <= 12; ++month) {
        now = At(2026, month, 15);
        cal.Init(400, 300);
        cal.Render(fb.data(), 400, 300);
        cal.HandleInput(Ev(ButtonEvent::kBootClick));
        cal.Render(fb.data(), 400, 300);
    }
}

}  // namespace

int main() {
    TestDates();
    TestText();
    TestStore();
    TestLoadSanitizes();
    TestFilePersistence();
    TestCalendarApp();
    if (g_failed) {
        fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("test_reminders: OK\n");
    return 0;
}
