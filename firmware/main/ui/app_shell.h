/**
 * @file app_shell.h
 * @brief Hardware-free navigation rules shared by the firmware and the PC simulator.
 *
 * The shell decides *what* a button press means at the app level (open an app from Home, long-press
 * BOOT to return Home). The platform supplies the Host callbacks that actually switch pages and start
 * the hotspots, so both sides run exactly the same decisions.
 */

#ifndef UI_APP_SHELL_H
#define UI_APP_SHELL_H

#include <functional>

#include "ui/app_registry.h"
#include "ui/page_ids.h"
#include "ui/renderers/rawdraw/page_renderer.h"

namespace ui {

class AppShell {
public:
    /** Platform side effects. Unset callbacks are ignored. */
    struct Host {
        std::function<void(RawDrawPageId)> open_page;
        std::function<void()> start_ap_transfer;
        std::function<void()> start_wifi_config;
        /** Leave the photo-transfer hotspot page and return Home (stops the hotspot). */
        std::function<void()> stop_ap_transfer;
    };

    void SetHost(Host host) { host_ = std::move(host); }

    /** Run the action of an app chosen on the Home page. */
    void Launch(const AppEntry& app);

    /** Return to the Home page. */
    void GoHome();

    /**
     * App-level button rules that apply on every page, evaluated before the page sees the event.
     *   - BOOT long press: leave the current page and return Home (no effect on Home itself).
     * @return true if the event was consumed.
     */
    bool HandleGlobalInput(RawDrawPageId current, const rawdraw::ButtonEvent& event);

private:
    Host host_;
};

}  // namespace ui

#endif  // UI_APP_SHELL_H
