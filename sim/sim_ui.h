// Host-side UI shell for the simulator.
//
// Mirrors the page management and button routing of firmware RawDrawUiManager (page switching with
// Init(), BOOT-long handling, routing to the active renderer). The app-level rules (Home page, app
// list, "BOOT long press goes Home") are NOT reimplemented here: they come from ui::AppShell,
// rawdraw::HomeRenderer and ui::GetApps(), the same code the firmware runs.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ui/app_shell.h"
#include "ui/page_ids.h"
#include "ui/renderers/rawdraw/ap_transfer_renderer.h"
#include "ui/renderers/rawdraw/ap_transfer_server.h"
#include "ui/renderers/rawdraw/home_renderer.h"
#include "ui/renderers/rawdraw/photo_detail_renderer.h"
#include "ui/renderers/rawdraw/photo_gallery.h"
#include "ui/renderers/rawdraw/settings_renderer.h"
#include "ui/renderers/rawdraw/voice_renderer.h"
#include "ui/renderers/rawdraw/ai_chat_renderer.h"
#include "ui/renderers/rawdraw/calendar_app_renderer.h"
#include "fake/voice_backend_fake.h"
#include "common/voice_recordings.h"
#include "ui/renderers/rawdraw/weather_detail_renderer.h"
#include "ui/renderers/rawdraw/weather_renderer.h"

class SimUi {
public:
    struct Options {
        // Fetch the canned weather at start (the device does so once Wi-Fi is up; the sim has no network).
        bool demo_weather = false;
        std::string lan_address = "127.0.0.1:8080";
        std::string voice_dir = "/tmp";  // where the fake recorder keeps rec_NNNN.wav
        std::string reminders_file = "/tmp/sim_reminders.json";  // calendar reminders (JSON)
        std::string today;  // "YYYY-MM-DD": fixed date for the calendar (default: system clock)
    };

    explicit SimUi(const Options& options);

    // Called from the HTTP service thread.
    void OnServerState(rawdraw::ApTransferServer::ServerState state, const std::string& message);
    void OnPhotosChanged();
    void OnImageReceived();
    bool ShowPhoto(const std::string& photo_id);
    void OnSlideshowChanged(int minutes);

    // Called from the command loop.
    // Key names: up down boot, "<name>long" for long press, "updownlong" for the UP+DOWN chord.
    // Returns false for an unknown key. *consumed is set to whether the active page handled it.
    bool HandleKey(const std::string& name, bool* consumed);
    void JumpTo(ui::RawDrawPageId page);  // debug helper, not a user action
    void Reboot();                        // like a restart: back to the Home page
    void Render(std::vector<uint8_t>* fb);

    ui::RawDrawPageId CurrentPage();
    static const char* PageName(ui::RawDrawPageId page);
    static bool PageFromName(const std::string& name, ui::RawDrawPageId* page);

private:
    void SwitchPage(ui::RawDrawPageId page);
    rawdraw::PageRenderer* RendererFor(ui::RawDrawPageId page);
    void BuildSettingsItems();
    void StartApTransferMode();
    void StopApTransferMode();
    void ShowWifiConfigPage();

    Options options_;
    std::recursive_mutex mu_;
    ui::RawDrawPageId current_ = ui::RawDrawPageId::Home;
    ui::AppShell shell_;
    bool ap_mode_running_ = false;
    int slideshow_minutes_ = 5;
    std::string shown_photo_;

    rawdraw::HomeRenderer home_;
    rawdraw::PhotoGalleryRenderer gallery_;
    rawdraw::PhotoDetailRenderer photo_detail_;
    rawdraw::WeatherRenderer weather_;
    rawdraw::WeatherDetailRenderer weather_detail_;
    rawdraw::SettingsRenderer settings_;
    rawdraw::ApTransferRenderer ap_;

    // Voice app: recordings go to a temp directory, capture/playback are faked.
    voice::RecordingStore voice_store_;
    voice::FakeVoiceBackend voice_backend_;
    rawdraw::VoiceRenderer voice_;
    rawdraw::CalendarAppRenderer calendar_;
    rawdraw::AiChatRenderer ai_chat_;  // state lives in ai::ChatService (started in the constructor)
};
