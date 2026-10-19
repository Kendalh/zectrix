#include "calendar_app_renderer.h"

#include <algorithm>
#include <cstdio>

#include "common/chinese_calendar.h"
#include "rawdraw/components/footer_bar.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t font_festival_12;

namespace rawdraw {

namespace {

constexpr int kTitleH = 24;
constexpr int kWeekH = 18;
constexpr int kRowH = 34;
constexpr int kMarginX = 14;
constexpr int kBellW = 9;

const char* const kWeekNames[] = {"日", "一", "二", "三", "四", "五", "六"};
const char* const kWeekNamesLong[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

// 9x9 bell; '#' is ink.
const char* const kBell[] = {
    "....#....",
    "..#####..",
    ".#######.",
    ".#######.",
    ".#######.",
    "#########",
    "#########",
    ".........",
    "...###...",
};

void DrawBell(uint8_t* fb, int width, int x, int y, Color color) {
    for (int row = 0; row < 9; ++row) {
        for (int col = 0; col < kBellW; ++col) {
            if (kBell[row][col] == '#') set_pixel(fb, width, x + col, y + row, color);
        }
    }
}

size_t Utf8Len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    return 4;
}

// Break `text` into lines no wider than max_w (at most max_lines; the last one is cut off).
std::vector<std::string> WrapText(const std::string& text, const lv_font_t* font, int max_w, size_t max_lines) {
    std::vector<std::string> lines;
    std::string line;
    int line_w = 0;
    for (size_t i = 0; i < text.size();) {
        const size_t len = std::min(Utf8Len(static_cast<unsigned char>(text[i])), text.size() - i);
        const std::string ch = text.substr(i, len);
        const int w = MeasureTextWidth(ch.c_str(), font);
        if (line_w + w > max_w && !line.empty()) {
            lines.push_back(line);
            if (lines.size() == max_lines) return lines;
            line.clear();
            line_w = 0;
        }
        line += ch;
        line_w += w;
        i += len;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

}  // namespace

CalendarAppRenderer::CalendarAppRenderer()
    : store_(&reminders::Store::Shared()),
      clock_([]() { return time(nullptr); }),
      font_(&SourceHanSansSC_Regular_slim),
      title_font_(&SourceHanSansSC_Medium_slim),
      festival_font_(&font_festival_12) {}

CalendarAppRenderer::~CalendarAppRenderer() = default;

CalendarAppRenderer::Today CalendarAppRenderer::Now() const {
    const time_t now = clock_();
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    return {tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday};
}

void CalendarAppRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    const Today today = Now();
    view_year_ = today.year;
    view_month_ = today.month;
    mode_ = Mode::Month;
    first_item_ = 0;
}

bool CalendarAppRenderer::HandleInput(const ButtonEvent& event) {
    const Today today = Now();

    if (mode_ == Mode::Today) {
        const int count = static_cast<int>(store_->Get(today.year, today.month, today.day).size());
        switch (event.type) {
            case ButtonEvent::kUpClick:
                first_item_ = std::max(0, first_item_ - 1);
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kDownClick:
                first_item_ = std::min(std::max(0, count - 1), first_item_ + 1);
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kBootClick:
                mode_ = Mode::Month;
                needs_full_refresh_ = true;
                return true;
            default:
                return false;
        }
    }

    switch (event.type) {
        case ButtonEvent::kUpClick:
        case ButtonEvent::kDownClick: {
            const int step = event.type == ButtonEvent::kUpClick ? -1 : 1;
            int month = view_month_ + step;
            int year = view_year_;
            if (month < 1) {
                month = 12;
                --year;
            } else if (month > 12) {
                month = 1;
                ++year;
            }
            if (year < reminders::kMinYear || year > reminders::kMaxYear) return true;  // end of range
            view_year_ = year;
            view_month_ = month;
            needs_full_refresh_ = true;
            return true;
        }
        case ButtonEvent::kBootClick:
            if (view_year_ != today.year || view_month_ != today.month) {
                view_year_ = today.year;
                view_month_ = today.month;
            } else {
                mode_ = Mode::Today;
                first_item_ = 0;
            }
            needs_full_refresh_ = true;
            return true;
        default:
            return false;
    }
}

void CalendarAppRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    FillRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, WHITE);
    if (mode_ == Mode::Today) {
        RenderToday(fb, width, height);
    } else {
        RenderMonth(fb, width, height);
    }
    needs_full_refresh_ = false;
}

void CalendarAppRenderer::RenderMonth(uint8_t* fb, int width, int height) {
    const Today today = Now();

    char title[32];
    snprintf(title, sizeof(title), "%d年%d月", view_year_, view_month_);
    const int title_cy = Style::kStatusBarHeight + kTitleH / 2 - 3;  // keep clear of the weekday band
    DrawText(fb, width, (width - MeasureTextWidth(title, title_font_)) / 2,
             InkCenteredTextTopY(title_font_, title, title_cy, 0), title, title_font_, BLACK);

    // Weekday band: black with white letters, weekend letters in yellow.
    const int week_top = Style::kStatusBarHeight + kTitleH;
    FillRect(fb, width, {0, week_top, width, kWeekH}, BLACK);
    for (int col = 0; col < 7; ++col) {
        const int x0 = col * width / 7;
        const int x1 = (col + 1) * width / 7;
        const bool weekend = col == 0 || col == 6;
        DrawText(fb, width, x0 + (x1 - x0 - MeasureTextWidth(kWeekNames[col], font_)) / 2,
                 InkCenteredTextTopY(font_, kWeekNames[col], week_top + kWeekH / 2, 0), kWeekNames[col], font_,
                 weekend ? YELLOW : WHITE);
    }
    const int grid_top = week_top + kWeekH;

    const int first_weekday = reminders::Weekday(view_year_, view_month_, 1);
    const int days = reminders::DaysInMonth(view_year_, view_month_);
    const unsigned marked = store_->DaysWithReminders(view_year_, view_month_);
    const bool current_month = view_year_ == today.year && view_month_ == today.month;

    for (int day = 1; day <= days; ++day) {
        const int slot = first_weekday + day - 1;
        const int col = slot % 7;
        const int row = slot / 7;
        const int x0 = col * width / 7;
        const int cell_w = (col + 1) * width / 7 - x0;
        const int y0 = grid_top + 1 + row * kRowH;
        const bool is_today = current_month && day == today.day;
        const bool weekend = col == 0 || col == 6;
        // A festival name goes under the number, so the number moves up on those days.
        const char* festival = cncal::MainFestival(view_year_, view_month_, day);

        // Colors: today = yellow block with black text, weekend numbers red, festival names yellow, rest black.
        Color number_color = weekend ? RED : BLACK;
        Color label_color = YELLOW;
        Color bell_color = RED;
        if (is_today) {
            const int pad = festival ? 0 : 2;
            DrawRoundRect(fb, width, height, {x0 + 3, y0 + pad, cell_w - 6, kRowH - 2 * pad}, Style::kBorderRadiusMD,
                          YELLOW, YELLOW, 1);
            number_color = BLACK;  // red/yellow on yellow would not read
            label_color = BLACK;
            bell_color = BLACK;
        }

        char num[16];
        snprintf(num, sizeof(num), "%d", day);
        DrawText(fb, width, x0 + (cell_w - MeasureTextWidth(num, font_)) / 2,
                 InkCenteredTextTopY(font_, num, y0 + (festival ? 9 : kRowH / 2), 0), num, font_, number_color);
        if (festival) {
            DrawText(fb, width, x0 + (cell_w - MeasureTextWidth(festival, festival_font_)) / 2,
                     InkCenteredTextTopY(festival_font_, festival, y0 + 25, 0), festival, festival_font_, label_color);
        }
        if (marked & (1u << day)) DrawBell(fb, width, x0 + cell_w - kBellW - 7, y0 + 6, bell_color);
    }

    FooterBar footer;
    footer.SetBounds(width, height);
    footer.SetText("UP/DN翻月", nullptr, current_month ? "BOOT今日提醒" : "BOOT回本月");
    footer.Draw(fb, width, height);
}

void CalendarAppRenderer::RenderToday(uint8_t* fb, int width, int height) {
    const Today today = Now();
    const auto items = store_->Get(today.year, today.month, today.day);
    const int weekday = reminders::Weekday(today.year, today.month, today.day);

    char title[48];
    snprintf(title, sizeof(title), "%d月%d日 %s", today.month, today.day, kWeekNamesLong[weekday]);
    const int title_cy = Style::kStatusBarHeight + kTitleH / 2;
    DrawText(fb, width, kMarginX, InkCenteredTextTopY(title_font_, title, title_cy, 0), title, title_font_,
             (weekday == 0 || weekday == 6) ? RED : BLACK);
    char count_text[24];
    snprintf(count_text, sizeof(count_text), "今日提醒 %d", static_cast<int>(items.size()));
    const int count_x = width - kMarginX - MeasureTextWidth(count_text, font_);
    DrawText(fb, width, count_x, InkCenteredTextTopY(font_, count_text, title_cy, 0), count_text, font_, BLACK);

    // Today's festivals in yellow between the date and the count (dropped if they would collide).
    const char* festivals[cncal::kMaxFestivalsPerDay];
    const int festival_count =
        cncal::FestivalsOn(today.year, today.month, today.day, festivals, cncal::kMaxFestivalsPerDay);
    std::string festival_text;
    for (int i = 0; i < festival_count; ++i) festival_text += (i ? " " : "") + std::string(festivals[i]);
    const int festival_x = kMarginX + MeasureTextWidth(title, title_font_) + 12;
    if (!festival_text.empty() &&
        festival_x + MeasureTextWidth(festival_text.c_str(), festival_font_) < count_x - 8) {
        DrawText(fb, width, festival_x, InkCenteredTextTopY(festival_font_, festival_text.c_str(), title_cy, 0),
                 festival_text.c_str(), festival_font_, YELLOW);
    }

    const int list_top = Style::kStatusBarHeight + kTitleH + 4;
    FillRect(fb, width, {kMarginX, list_top, width - 2 * kMarginX, 2}, RED);

    const int list_bottom = height - Style::kFooterBarHeight - 4;
    if (items.empty()) {
        const char* msg = "今天没有提醒";
        const std::string hint =
            "在浏览器打开 " + (host_.empty() ? std::string("设备地址") : "http://" + host_) + "/calendar";
        const int cy = (list_top + list_bottom) / 2;
        DrawText(fb, width, (width - MeasureTextWidth(msg, title_font_)) / 2,
                 InkCenteredTextTopY(title_font_, msg, cy - 14, 0), msg, title_font_, BLACK);
        DrawText(fb, width, std::max(4, (width - MeasureTextWidth(hint.c_str(), font_)) / 2),
                 InkCenteredTextTopY(font_, hint.c_str(), cy + 16, 0), hint.c_str(), font_, RED);
    }

    constexpr int kLineH = 22;
    constexpr int kItemPad = 8;
    constexpr int kTextX = kMarginX + 16;
    int y = list_top + 6;
    int index = std::min(first_item_, std::max(0, static_cast<int>(items.size()) - 1));
    for (; index < static_cast<int>(items.size()); ++index) {
        const auto lines = WrapText(items[index], font_, width - kTextX - kMarginX, 3);
        const int item_h = static_cast<int>(lines.size()) * kLineH + kItemPad;
        if (y + item_h > list_bottom) break;
        DrawCircle(fb, width, {kMarginX + 5, y + kLineH / 2}, 3, RED);
        for (size_t i = 0; i < lines.size(); ++i) {
            DrawText(fb, width, kTextX,
                     InkCenteredTextTopY(font_, lines[i].c_str(), y + kLineH / 2 + static_cast<int>(i) * kLineH, 0),
                     lines[i].c_str(), font_, BLACK);
        }
        y += item_h;
    }
    const bool more_below = index < static_cast<int>(items.size());
    if (first_item_ > 0) DrawText(fb, width, width - kMarginX - 8, list_top + 4, "^", font_, RED);
    if (more_below) DrawText(fb, width, width - kMarginX - 8, list_bottom - 14, "v", font_, RED);

    FooterBar footer;
    footer.SetBounds(width, height);
    footer.SetText("UP/DN滚动", nullptr, "BOOT返回月历");
    footer.Draw(fb, width, height);
}

}  // namespace rawdraw
