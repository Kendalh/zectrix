// fa_settings.h pulls in the real <lvgl.h>; it has to come before anything that includes
// rawdraw/font_engine.h (which defines its own lv_font_t when LVGL is not already visible),
// otherwise the two definitions clash. rawdraw_ui_manager.cc relies on the same ordering.
#include "components/78__xiaozhi-fonts/include/fa_settings.h"

#include "ui/app_registry.h"

namespace ui {

namespace {
// weather_icons.h declares the weather fonts with C linkage, which conflicts with the C++ declarations
// in rawdraw/font_engine.h, so the one glyph needed here is defined locally (FontAwesome U+F185, sun).
constexpr char kWeatherSunGlyph[] = "\xef\x86\x85";
}  // namespace

const std::vector<AppEntry>& GetApps() {
    static const std::vector<AppEntry> kApps = {
        {"gallery", "相册", "浏览照片", FA_SETTINGS_IMAGE, &fa_settings_16,
         AppAction::OpenPage, RawDrawPageId::Gallery},
        {"weather", "天气", "天气预报", kWeatherSunGlyph, &weather_icons_16,
         AppAction::OpenPage, RawDrawPageId::Weather},
        {"voice", "语音", "录音与播放", FA_SETTINGS_VOLUME, &fa_settings_16,
         AppAction::OpenPage, RawDrawPageId::Voice},
        {"transfer", "传图", "热点传照片", FA_SETTINGS_SYNC, &fa_settings_16,
         AppAction::StartApTransfer, RawDrawPageId::APTransfer},
        {"wifi_config", "配网", "配置 Wi-Fi", FA_SETTINGS_WIFI, &fa_settings_16,
         AppAction::StartWifiConfig, RawDrawPageId::APTransfer},
        {"settings", "设置", "Wi-Fi 与系统", FA_SETTINGS_GEAR, &fa_settings_16,
         AppAction::OpenPage, RawDrawPageId::Settings},
    };
    return kApps;
}

}  // namespace ui
