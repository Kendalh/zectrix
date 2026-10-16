#include "ui/app_shell.h"

namespace ui {

void AppShell::Launch(const AppEntry& app) {
    switch (app.action) {
        case AppAction::OpenPage:
            if (host_.open_page) host_.open_page(app.page);
            break;
        case AppAction::StartApTransfer:
            if (host_.start_ap_transfer) host_.start_ap_transfer();
            break;
        case AppAction::StartWifiConfig:
            if (host_.start_wifi_config) host_.start_wifi_config();
            break;
    }
}

void AppShell::GoHome() {
    if (host_.open_page) host_.open_page(RawDrawPageId::Home);
}

bool AppShell::HandleGlobalInput(RawDrawPageId current, const rawdraw::ButtonEvent& event) {
    if (event.type != rawdraw::ButtonEvent::kBootLongPress || current == RawDrawPageId::Home) {
        return false;
    }
    if (current == RawDrawPageId::APTransfer && host_.stop_ap_transfer) {
        host_.stop_ap_transfer();  // the hotspot must be torn down before the page changes
    } else {
        GoHome();
    }
    return true;
}

}  // namespace ui
