/**
 * @file calendar_app_renderer.h
 * @brief Calendar app: month grid with a bell on dates that have reminders, and today's reminders.
 *
 * Buttons (same on the device and in the simulator):
 *  - Month view: UP/DOWN = previous/next month. BOOT = jump back to the current month; when the current
 *    month is already shown, BOOT opens today's reminders.
 *  - Today view: UP/DOWN = scroll, BOOT = back to the month view.
 *
 * Reminders come from reminders::Store (edited in the browser at /calendar). Distinct from the older
 * CalendarRenderer (lunar calendar, not reachable from Home).
 */

#ifndef RAWDRAW_CALENDAR_APP_RENDERER_H
#define RAWDRAW_CALENDAR_APP_RENDERER_H

#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "common/reminders.h"
#include "page_renderer.h"

namespace rawdraw {

class CalendarAppRenderer : public PageRenderer {
public:
    using Clock = std::function<time_t()>;

    CalendarAppRenderer();
    ~CalendarAppRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    /** Address of the device web page shown in the hints, e.g. "192.168.1.20:8080" (no scheme, no path). */
    void SetAddress(const std::string& host) { host_ = host; }

    // Test hooks: defaults are the shared store and the system clock.
    void SetStore(reminders::Store* store) { store_ = store; }
    void SetClock(Clock clock) { clock_ = std::move(clock); }

    // State, for tests and the simulator.
    int ViewYear() const { return view_year_; }
    int ViewMonth() const { return view_month_; }
    bool ShowingToday() const { return mode_ == Mode::Today; }

private:
    enum class Mode { Month, Today };

    struct Today {
        int year, month, day;
    };

    Today Now() const;
    void RenderMonth(uint8_t* fb, int width, int height);
    void RenderToday(uint8_t* fb, int width, int height);

    reminders::Store* store_;
    Clock clock_;
    std::string host_;
    Mode mode_ = Mode::Month;
    int view_year_ = 2026;
    int view_month_ = 1;
    int first_item_ = 0;  // Today view: first reminder shown
    const lv_font_t* font_;
    const lv_font_t* title_font_;
    const lv_font_t* festival_font_;  // 12 px, festival names only (see font_festival_12)
};

}  // namespace rawdraw

#endif  // RAWDRAW_CALENDAR_APP_RENDERER_H
