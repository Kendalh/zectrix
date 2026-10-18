/**
 * @file ai_chat_renderer.h
 * @brief AI Q&A page: shows the current session's conversation, one page at a time.
 *
 * Questions are typed on the device web page (/ai); the panel only displays them, because the device
 * has three buttons and no keyboard. The answer arrives whole (non-streaming) and the page is
 * redrawn once when it is complete: the slow panel is not refreshed for the "thinking" state.
 *
 * Questions can be spoken: hold UP to record (kUpLongPress), release to send (kUpLongRelease). The recording is
 * made by the Voice app's backend, transcribed by ai::ChatService (speech-to-text) and asked like a typed question.
 *
 * Conversation view: UP/DOWN previous/next page, UP long press = record a question (release sends it),
 * BOOT opens the session list, DOWN long press starts a new session.
 * Session list: UP/DOWN select ("+ 新会话" is the first row), BOOT opens the selected one, DOWN long press deletes it.
 * BOOT long press leaves the page (global, AppShell). Everything is done on the device; the web page /ai only
 * holds the API keys. State lives in ai::ChatService, shared with the simulator.
 */

#ifndef RAWDRAW_AI_CHAT_RENDERER_H
#define RAWDRAW_AI_CHAT_RENDERER_H

#include <string>
#include <vector>

#include "common/ai_chat.h"
#include "common/voice_backend.h"
#include "common/voice_recordings.h"
#include "page_renderer.h"

namespace rawdraw {

class AiChatRenderer : public PageRenderer {
public:
    AiChatRenderer();
    ~AiChatRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    /** Microphone side, shared with the Voice app. Both must outlive the renderer. */
    void SetBackend(voice::VoiceBackend* backend, voice::RecordingStore* store) {
        backend_ = backend;
        store_ = store;
    }

    /** Page left: an unfinished recording is stopped and no longer treated as a question. */
    void OnLeave();

    /**
     * Recording saved by the backend (called on its saving thread). @return true if it was recorded for a question
     * (it is transcribed and deleted, so it does not show up in the Voice list), false for the Voice app's own.
     */
    bool OnRecordingSaved(const std::string& path);

    // Start and end of recording are signalled by beeps and the LED, like in the Voice app: no 15 s panel refresh.
    bool SkipRefreshAfterInput() const override { return silent_input_; }

    /** Address of the device web page shown in the hints, e.g. "192.168.1.20" (no scheme, no path). */
    void SetAddress(const std::string& host) { host_ = host; }

    /**
     * Call when ai::ChatService reports a change. @return true if the panel should be redrawn now: the
     * visible content changed and no question is awaiting its answer (no refresh for "thinking").
     */
    bool NeedsRefreshAfterChange();

private:
    struct Line {
        std::string text;
        bool question = false;
        bool secondary = false;  // hints, thinking, errors
    };

    void RenderList(uint8_t* fb, int width, int height);
    void BuildLines(const ai::Session& session, bool has_key, bool started, int content_width);
    std::string Signature(const ai::Session& session, bool has_key, bool started) const;
    int LinesPerPage() const;
    int PageCount() const;
    void WrapInto(const std::string& text, bool question, bool secondary, int width, const std::string& prefix = "");

    std::string host_;
    std::vector<Line> lines_;
    int top_line_ = 0;
    int answer_start_line_ = 0;   // first line of the newest question
    int shown_session_ = 0;
    size_t shown_count_ = 0;
    std::string shown_sig_;       // what the panel currently shows (for NeedsRefreshAfterChange)
    bool in_input_ = false;
    bool list_mode_ = false;  // session list instead of the conversation
    int list_sel_ = 0;        // 0 = "+ 新会话", i + 1 = sessions[i]
    bool silent_input_ = false;
    bool recording_for_question_ = false;  // this page started the capture in progress / being saved
    int recording_session_ = 0;
    voice::VoiceBackend* backend_ = nullptr;
    voice::RecordingStore* store_ = nullptr;
    const lv_font_t* font_;
    const lv_font_t* title_font_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_AI_CHAT_RENDERER_H
