/**
 * @file page_ids.h
 * @brief Page identifiers for the rawdraw UI, kept free of hardware headers so the PC
 *        simulator and the firmware share one definition.
 */

#ifndef UI_PAGE_IDS_H
#define UI_PAGE_IDS_H

namespace ui {

/**
 * @brief Page identifiers for the rawdraw UI
 *
 * Only pages with rawdraw renderers are included here.
 * Separate from ui::PageId to avoid conflict with LVGL UiManager.
 */
enum class RawDrawPageId {
    Chat = 0,
    Ebook = 2,
    Wifi = 3,
    Settings = 4,
    Gallery = 5,
    Weather = 6,
    News = 7,
    WeatherDetail = 8,
    PhotoDetail = 9,
    LifeBar = 10,
    Almanac = 11,
    Log = 12,
    YearProgress = 13,
    Calendar = 14,
    FontDebug = 15,
    FontMetrics = 16,
    APTransfer = 17,
    Home = 18,
    Voice = 19,
    AiChat = 20,
    CalendarApp = 21,  ///< Month calendar with reminders (the older `Calendar` page is not on Home)
    Count,
};

}  // namespace ui

#endif  // UI_PAGE_IDS_H
