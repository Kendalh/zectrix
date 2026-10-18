#include "sim_ui.h"

#include <cstdio>

#include "sim_fixtures.h"

using ui::RawDrawPageId;

namespace {
// Indices into the settings list built by BuildSettingsItems() (same as application.cc).
constexpr int kSettingsSlideshowIndex = 3;
constexpr int kSettingsWifiIndex = 5;
constexpr int kSettingsHttpServerIndex = 6;
constexpr int kSettingsLanIpIndex = 7;

std::string MinutesLabel(int minutes) { return minutes <= 0 ? "关闭" : std::to_string(minutes) + "min"; }
}  // namespace

SimUi::SimUi(const Options& options) : options_(options) {
    home_.SetLaunchCallback([this](const ui::AppEntry& app) { shell_.Launch(app); });

    ui::AppShell::Host host;
    host.open_page = [this](RawDrawPageId page) { SwitchPage(page); };
    host.start_ap_transfer = [this]() { StartApTransferMode(); };
    host.stop_ap_transfer = [this]() { StopApTransferMode(); };
    host.start_wifi_config = [this]() { ShowWifiConfigPage(); };
    shell_.SetHost(std::move(host));

    BuildSettingsItems();
    home_.Init(sim::kW, sim::kH);

    // Same as RawDrawUiManager::StartWeatherService(): the weather "task" pushes data to both pages.
    weather_api_start([this](const WeatherData& data) {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        weather_.Update(data);
        weather_detail_.Update(data);
    });
    // The device fetches once Wi-Fi is up; the simulator only does so when asked to preview the layout.
    if (options_.demo_weather) weather_api_fetch_now();
}

const char* SimUi::PageName(RawDrawPageId page) {
    switch (page) {
        case RawDrawPageId::Home: return "home";
        case RawDrawPageId::Gallery: return "gallery";
        case RawDrawPageId::PhotoDetail: return "photo_detail";
        case RawDrawPageId::Weather: return "weather";
        case RawDrawPageId::WeatherDetail: return "weather_detail";
        case RawDrawPageId::Settings: return "settings";
        case RawDrawPageId::APTransfer: return "ap_transfer";
        default: return "unknown";
    }
}

bool SimUi::PageFromName(const std::string& name, RawDrawPageId* page) {
    for (RawDrawPageId p : {RawDrawPageId::Home, RawDrawPageId::Gallery, RawDrawPageId::PhotoDetail,
                            RawDrawPageId::Weather, RawDrawPageId::WeatherDetail,
                            RawDrawPageId::Settings, RawDrawPageId::APTransfer}) {
        if (name == PageName(p)) {
            *page = p;
            return true;
        }
    }
    return false;
}

rawdraw::PageRenderer* SimUi::RendererFor(RawDrawPageId page) {
    switch (page) {
        case RawDrawPageId::Home: return &home_;
        case RawDrawPageId::Gallery: return &gallery_;
        case RawDrawPageId::PhotoDetail: return &photo_detail_;
        case RawDrawPageId::Weather: return &weather_;
        case RawDrawPageId::WeatherDetail: return &weather_detail_;
        case RawDrawPageId::Settings: return &settings_;
        case RawDrawPageId::APTransfer: return &ap_;
        default: return nullptr;
    }
}

RawDrawPageId SimUi::CurrentPage() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return current_;
}

// Same order as RawDrawUiManager::SwitchPage: Init() the new renderer, then make it current.
void SimUi::SwitchPage(RawDrawPageId page) {
    if (page == current_) return;
    if (auto* r = RendererFor(page)) {
        r->Init(sim::kW, sim::kH);
        r->MarkFullRefresh();
    }
    current_ = page;
}

void SimUi::JumpTo(RawDrawPageId page) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    SwitchPage(page);
}

void SimUi::Reboot() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    shown_photo_.clear();
    ap_mode_running_ = false;
    current_ = RawDrawPageId::Home;
    home_.SetSelectedIndex(0);  // a real reboot forgets the last selection
    home_.Init(sim::kW, sim::kH);
}

void SimUi::StartApTransferMode() {
    ap_.UseDefaultTransferInstructions();
    ap_mode_running_ = true;
    SwitchPage(RawDrawPageId::APTransfer);
    // On the device the AP start task reports this a moment after the page appears.
    ap_.SetState(rawdraw::ApTransferRenderer::kWaitingForConnection, "192.168.4.1");
}

void SimUi::StopApTransferMode() {
    ap_mode_running_ = false;
    SwitchPage(RawDrawPageId::Home);
}

void SimUi::ShowWifiConfigPage() {
    // Placeholders: the real SSID/password come from WifiManager on the device.
    ap_.SetInstructionContent("WiFi 配网", "ZecTrix", "", "http://192.168.4.1",
                              "连接热点后打开页面配置 WiFi", "长按 BOOT 退出");
    ap_.SetState(rawdraw::ApTransferRenderer::kWaitingForConnection, "192.168.4.1");
    SwitchPage(RawDrawPageId::APTransfer);
}

bool SimUi::HandleKey(const std::string& name, bool* consumed) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    *consumed = false;

    if (name == "updownlong") {  // Application::OnWifiConfigComboLongPress
        ShowWifiConfigPage();
        *consumed = true;
        return true;
    }
    rawdraw::ButtonEvent event;
    if (!sim::ParseKey(name, &event)) return false;

    if (event.type == rawdraw::ButtonEvent::kBootLongPress) {
        // Same order as RawDrawUiManager::HandleInput.
        if (ap_mode_running_ || current_ == RawDrawPageId::APTransfer) {
            StopApTransferMode();
            *consumed = true;
            return true;
        }
        if (shell_.HandleGlobalInput(current_, event)) {
            *consumed = true;
            return true;
        }
    }
    if (auto* r = RendererFor(current_)) *consumed = r->HandleInput(event);
    return true;
}

void SimUi::Render(std::vector<uint8_t>* fb) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    fb->assign(sim::kBpr * sim::kH, 0x55);
    if (auto* r = RendererFor(current_)) r->Render(fb->data(), sim::kW, sim::kH);
}

// ---- events from the HTTP service (mirrors the callbacks RawDrawUiManager installs) ----

void SimUi::OnServerState(rawdraw::ApTransferServer::ServerState state, const std::string& message) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    using S = rawdraw::ApTransferServer;
    using R = rawdraw::ApTransferRenderer;
    switch (state) {
        case S::kClientConnected: ap_.SetState(R::kClientConnected, message); break;
        case S::kReceivingImage:
        case S::kProcessingImage: break;  // skipped on the device: too slow to refresh the panel
        case S::kImageSaved: ap_.SetState(R::kComplete, message); break;
        case S::kError: ap_.SetState(R::kError, message); break;
        case S::kApStarted:
        case S::kStopped:
        default: ap_.SetState(R::kWaitingForConnection, message); break;
    }
}

void SimUi::OnPhotosChanged() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    gallery_.RefreshPhotoList();
}

void SimUi::OnImageReceived() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    gallery_.RefreshPhotoList();
    const int count = gallery_.GetPhotoCount();
    if (count > 0) gallery_.SetSelectedIndex(count - 1);
}

bool SimUi::ShowPhoto(const std::string& photo_id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (photo_id.empty() || !photo_exists(photo_id.c_str())) return false;
    // Same order as RawDrawUiManager::ShowPhotoById: switch first (Init() resets the gallery), then select.
    SwitchPage(RawDrawPageId::Gallery);
    gallery_.RefreshPhotoList();
    if (!gallery_.SetSelectedById(photo_id.c_str())) return false;
    gallery_.EnterFullscreenMode();
    return true;
}

void SimUi::OnSlideshowChanged(int minutes) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    slideshow_minutes_ = minutes;
    settings_.UpdateItem(kSettingsSlideshowIndex, MinutesLabel(minutes));
}

// Mirrors the settings list built in Application (application.cc). Keep labels and order in sync.
void SimUi::BuildSettingsItems() {
    using rawdraw::SettingsItemDef;
    using rawdraw::SettingsItemType;
    std::vector<SettingsItemDef> items;
    items.push_back({"系统", "", nullptr, SettingsItemType::Section, false});
    items.push_back({"重启", "执行", nullptr, SettingsItemType::Action, false, [this]() { Reboot(); }});
    items.push_back({"相册", "", nullptr, SettingsItemType::Section, false});
    items.push_back({"轮播间隔", MinutesLabel(slideshow_minutes_), nullptr, SettingsItemType::Action, false,
                     [this]() {
                         static const int kOptions[] = {0, 5, 10, 30};
                         for (size_t i = 0; i < 4; ++i) {
                             if (kOptions[i] == slideshow_minutes_) {
                                 slideshow_minutes_ = kOptions[(i + 1) % 4];
                                 break;
                             }
                         }
                         settings_.UpdateItem(kSettingsSlideshowIndex, MinutesLabel(slideshow_minutes_));
                     }});
    items.push_back({"网络", "", nullptr, SettingsItemType::Section, false});
    items.push_back({"Wi-Fi", "已连接", nullptr, SettingsItemType::Checkbox, true,
                     [this]() { settings_.UpdateItem(kSettingsWifiIndex, "(模拟)"); }});
    items.push_back({"局域网服务", options_.lan_address, nullptr, SettingsItemType::Checkbox, true,
                     [this]() { settings_.UpdateItem(kSettingsHttpServerIndex, "(模拟)"); }});
    items.push_back({"局域网IP", options_.lan_address, nullptr, SettingsItemType::Normal, false});
    items.push_back({"省电模式", "手动进入", nullptr, SettingsItemType::Action, false,
                     []() { fprintf(stderr, "[sim] deep sleep requested (ignored)\n"); }});
    items.push_back({"关于", "", nullptr, SettingsItemType::Section, false});
    items.push_back({"固件", PROJECT_VER, nullptr, SettingsItemType::Normal, false});
    (void)kSettingsLanIpIndex;
    settings_.SetItems(items);
    settings_.SetFirmwareVersion("v" PROJECT_VER);
    settings_.SetDeviceInfo("00:00:00:00:00:00", "ESP32-S3");
}
