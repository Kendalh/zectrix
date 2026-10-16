/**
 * @file app_registry.h
 * @brief The list of apps shown on the Home page.
 *
 * To add an app: add one entry in app_registry.cc (and, for OpenPage apps, make sure the page has a
 * renderer wired into RawDrawUiManager). The Home page, its navigation and the PC simulator all read
 * this list, so nothing else needs to change.
 */

#ifndef UI_APP_REGISTRY_H
#define UI_APP_REGISTRY_H

#include <vector>

#include "rawdraw/font_engine.h"
#include "ui/page_ids.h"

namespace ui {

/** What happens when the user confirms an app on the Home page. */
enum class AppAction {
    OpenPage,         ///< Switch to `page`.
    StartApTransfer,  ///< Start the photo-transfer hotspot (InkScreen-AP) and show its page.
    StartWifiConfig,  ///< Start the Wi-Fi provisioning hotspot and show its page.
};

struct AppEntry {
    const char* id;            ///< Stable identifier (used by tests and the simulator).
    const char* title;         ///< Shown in the list.
    const char* hint;          ///< Short description shown on the right of the row.
    const char* icon;          ///< UTF-8 glyph in `icon_font`.
    const lv_font_t* icon_font;
    AppAction action;
    RawDrawPageId page;        ///< Target for AppAction::OpenPage; ignored otherwise.
};

/** Apps in display order. */
const std::vector<AppEntry>& GetApps();

}  // namespace ui

#endif  // UI_APP_REGISTRY_H
