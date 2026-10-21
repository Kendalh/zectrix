/**
 * @file ai_paint_renderer.h
 * @brief AI painter page: speak a subject, the four-colour picture fills the panel; redraw it or save it to the album.
 *
 * Hold UP (kUpLongPress) to record what to paint, release (kUpLongRelease) to send it: the Voice app's backend records,
 * ai::paint::PaintService transcribes, paints and converts (see common/ai_paint.h). The picture is shown full screen
 * with a one-line hint bar; a line above it names what was painted (shown with the picture, never while working), and
 * shows errors and "saved" notes too.
 *
 * Picture view: UP click = redraw (same subject, new picture), DOWN click = save to the album, hold UP = speak a new
 * subject. No picture yet: the page explains how to start. BOOT long press leaves the page (global, AppShell).
 * Like the AI Q&A page the slow panel is refreshed once when painting starts and once when it finishes, not per step.
 */

#ifndef RAWDRAW_AI_PAINT_RENDERER_H
#define RAWDRAW_AI_PAINT_RENDERER_H

#include <string>
#include <vector>

#include "common/ai_paint.h"
#include "common/voice_backend.h"
#include "common/voice_recordings.h"
#include "page_renderer.h"

namespace rawdraw {

class AiPaintRenderer : public PageRenderer {
public:
    AiPaintRenderer();
    ~AiPaintRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    /** Microphone side, shared with the Voice app. Both must outlive the renderer. */
    void SetBackend(voice::VoiceBackend* backend, voice::RecordingStore* store) {
        backend_ = backend;
        store_ = store;
    }

    /** Page left: an unfinished recording is stopped and no longer treated as a subject. */
    void OnLeave();

    /** Recording saved by the backend (called on its saving thread). @return true if it was recorded for a subject. */
    bool OnRecordingSaved(const std::string& path);

    // Recording start/stop are signalled by beeps and the LED, like in the Voice app: no 15 s panel refresh.
    bool SkipRefreshAfterInput() const override { return silent_input_; }

    /** The picture covers the whole panel: the shell hides its status bar and frame (like the fullscreen gallery). */
    bool IsFullscreenMode() const { return fullscreen_; }

    /** Address of the device web page shown in the hints (no scheme, no path). */
    void SetAddress(const std::string& host) { host_ = host; }

    /** Call when ai::paint::PaintService reports a change. @return true if the panel should be redrawn now. */
    bool NeedsRefreshAfterChange();

private:
    std::string Signature(const ai::paint::Status& s) const;
    void RenderText(uint8_t* fb, int width, int height, const ai::paint::Status& s, bool has_key, bool started);
    void RenderPicture(uint8_t* fb, int width, int height, const ai::paint::Status& s);
    void WrapInto(const std::string& text, int width, std::vector<std::string>* lines) const;

    std::string host_;
    std::string shown_sig_;
    std::string toast_;            // result of the last button action, shown once in the hint bar
    bool in_input_ = false;
    bool silent_input_ = false;
    bool fullscreen_ = false;
    bool recording_for_subject_ = false;
    voice::VoiceBackend* backend_ = nullptr;
    voice::RecordingStore* store_ = nullptr;
    const lv_font_t* font_;
    const lv_font_t* title_font_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_AI_PAINT_RENDERER_H
