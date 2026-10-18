/**
 * @file voice_renderer.h
 * @brief Voice app page: hold UP to record, list of recordings, BOOT plays the selected one.
 *
 * Keys: UP/DOWN select (rapid presses are coalesced into one refresh), DOWN long press deletes the
 * selected recording, BOOT play/stop, UP long press starts recording and releasing UP ends it
 * (kUpLongPress / kUpLongRelease). BOOT long press leaves the page (handled globally by AppShell).
 * Capture and playback go through voice::VoiceBackend so the renderer is shared with the simulator.
 */

#ifndef RAWDRAW_VOICE_RENDERER_H
#define RAWDRAW_VOICE_RENDERER_H

#include "page_renderer.h"
#include "common/voice_backend.h"
#include "common/voice_recordings.h"

namespace rawdraw {

class VoiceRenderer : public PageRenderer {
public:
    VoiceRenderer();
    ~VoiceRenderer() override;

    /** Both must outlive the renderer. */
    void SetBackend(voice::VoiceBackend* backend, voice::RecordingStore* store) {
        backend_ = backend;
        store_ = store;
    }

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    /** Called when the page is left: stops capture / playback. */
    void OnLeave();

    // Start / end of recording give feedback with the LED and beeps, and playback just plays: a 15 s panel
    // refresh for each would only show up when it is over. The list is redrawn once a file is saved.
    bool SkipRefreshAfterInput() const override { return silent_input_; }

    int GetSelectedIndex() const { return selected_index_; }

private:
    static constexpr int kVisibleRows = 5;

    void ClampWindow(int count);

    voice::VoiceBackend* backend_ = nullptr;
    voice::RecordingStore* store_ = nullptr;
    int selected_index_ = 0;
    int first_visible_ = 0;
    bool silent_input_ = false;
    bool select_newest_ = false;       // after a recording ends, select it once it is listed
    std::string playing_path_;
    const lv_font_t* font_;
    const lv_font_t* title_font_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_VOICE_RENDERER_H
