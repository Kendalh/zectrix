#include "ai_chat_renderer.h"

#include <algorithm>
#include <cstdio>

#include "rawdraw/components/footer_bar.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {
constexpr int kMarginX = 14;
constexpr int kHeaderHeight = 22;
constexpr int kLinePitch = 17;  // 16 px glyphs + 1: the body font is already the smallest CJK one, tighter lines fit more text

size_t Utf8Len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

// What the panel may show of a session: while a question is pending (transcribing or awaiting the answer) the screen stays
// exactly as before the question, so any refresh that happens meanwhile draws the same frame (the panel skips an unchanged
// frame) and the question and its answer appear together, in one refresh, when the answer arrives.
void HidePending(ai::Session* s) {
    if (!s->pending) return;
    if (!s->messages.empty() && s->messages.back().role == ai::Message::User) s->messages.pop_back();
    s->pending = false;
    s->transcribing = false;
}
}  // namespace

AiChatRenderer::AiChatRenderer()
    : font_(&SourceHanSansSC_Regular_slim), title_font_(&SourceHanSansSC_Medium_slim) {}

AiChatRenderer::~AiChatRenderer() = default;

void AiChatRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    top_line_ = 0;
    shown_session_ = 0;  // re-anchor on the newest question when the page is entered
    shown_count_ = 0;
    recording_for_question_ = false;
    list_mode_ = false;
}

void AiChatRenderer::OnLeave() {
    // Unclaim first: the backend may finish saving (and call OnRecordingSaved) before StopRecording() returns.
    const bool was_ours = recording_for_question_;
    recording_for_question_ = false;
    if (backend_ && was_ours && backend_->State() == voice::BackendState::Recording) backend_->StopRecording();
}

bool AiChatRenderer::OnRecordingSaved(const std::string& path) {
    if (!recording_for_question_) return false;
    recording_for_question_ = false;
    std::string error;
    auto& svc = ai::ChatService::Get();
    // Transcribes on its own task and deletes the file; on failure the file is ours to delete.
    if (!svc.AskVoice(recording_session_, path, &error)) {
        remove(path.c_str());
        svc.SetSessionError(recording_session_, error);
    }
    return true;
}

int AiChatRenderer::LinesPerPage() const {
    const int body_h = height_ - Style::kFooterBarHeight - (Style::kStatusBarHeight + kHeaderHeight) - 6;
    return std::max<int>(1, body_h / kLinePitch);
}

int AiChatRenderer::PageCount() const {
    const int per = LinesPerPage();
    return std::max(1, (static_cast<int>(lines_.size()) + per - 1) / per);
}

// Greedy wrap by glyph widths (CJK breaks anywhere; good enough for a 372 px column).
void AiChatRenderer::WrapInto(const std::string& text, bool question, bool secondary, int width,
                              const std::string& prefix) {
    std::string current = prefix;
    int current_w = prefix.empty() ? 0 : MeasureTextWidth(prefix.c_str(), font_);
    auto flush = [&]() {
        lines_.push_back({current, question, secondary});
        current.clear();
        current_w = 0;
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\n') {
            flush();
            ++i;
            continue;
        }
        const size_t n = std::min(Utf8Len(static_cast<unsigned char>(text[i])), text.size() - i);
        const std::string ch = text.substr(i, n);
        i += n;
        if (ch == "\r") continue;
        const int w = MeasureTextWidth(ch.c_str(), font_);
        if (current_w + w > width && !current.empty()) flush();
        current += ch;
        current_w += w;
    }
    if (!current.empty() || lines_.empty()) flush();
}

void AiChatRenderer::BuildLines(const ai::Session& session, bool has_key, bool started, int content_width) {
    lines_.clear();
    answer_start_line_ = 0;
    if (!started) {
        WrapInto("AI 服务未启动", false, false, content_width);
        WrapInto("请先连接 Wi-Fi。", false, true, content_width);
        return;
    }
    const std::string url = (host_.empty() ? std::string("设备地址") : "http://" + host_) + "/ai";
    if (!has_key) {
        WrapInto("还没有设置百炼 API Key", false, false, content_width);
        WrapInto("请用手机或电脑浏览器打开:", false, true, content_width);
        WrapInto(url, false, true, content_width);
        WrapInto("填入阿里云百炼 API Key。", false, true, content_width);
        return;
    }
    if (session.messages.empty() && !session.pending) {
        WrapInto("新会话", false, false, content_width);
        WrapInto("按住 UP 键说出问题，松开后发送。", false, true, content_width);
    }
    for (size_t i = 0; i < session.messages.size(); ++i) {
        const ai::Message& m = session.messages[i];
        if (m.role == ai::Message::User) {
            if (!lines_.empty()) lines_.push_back({"", false, true});  // gap between turns
            answer_start_line_ = static_cast<int>(lines_.size());
            WrapInto(m.text, true, false, content_width, "问: ");
        } else {
            WrapInto(m.text, false, false, content_width);
        }
    }
    // No "thinking" text while pending: the panel refreshes once, when the answer (or error) arrives.
    if (!session.last_error.empty()) {
        answer_start_line_ = static_cast<int>(lines_.size());
        WrapInto("提问失败: " + session.last_error, false, true, content_width);
    }
}

std::string AiChatRenderer::Signature(const ai::Session& s, bool has_key, bool started) const {
    return std::to_string(s.id) + "/" + std::to_string(s.messages.size()) + "/" + (s.pending ? "p" : "-") + "/" +
           s.last_error + "/" + (has_key ? "k" : "-") + (started ? "s" : "-");
}

bool AiChatRenderer::NeedsRefreshAfterChange() {
    if (in_input_) return false;  // the key handler already redraws
    auto& svc = ai::ChatService::Get();
    ai::Session s;
    if (!svc.Snapshot(svc.CurrentSessionId(), &s)) return false;
    if (s.pending) return false;  // wait for the whole answer; the panel is too slow for a "thinking" screen
    HidePending(&s);
    return Signature(s, svc.HasApiKey(), svc.Started()) != shown_sig_;
}

void AiChatRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    width_ = width;
    height_ = height;

    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle text_style = theme.Style(ThemeToken::TextPrimary);
    const PaintStyle secondary_style = theme.Style(ThemeToken::TextSecondary);

    const int body_top = Style::kStatusBarHeight;
    DrawStyledRect(fb, width, {0, body_top, width, height - body_top}, bg_style);

    auto& svc = ai::ChatService::Get();
    const bool started = svc.Started();
    const bool has_key = svc.HasApiKey();
    ai::Session session;
    const auto sessions = svc.List();
    int session_index = 0;
    if (started) {
        const int current = svc.CurrentSessionId();
        svc.Snapshot(current, &session);
        HidePending(&session);
        for (size_t i = 0; i < sessions.size(); ++i)
            if (sessions[i].id == current) session_index = static_cast<int>(i);
    }

    if (list_mode_ && started) {
        shown_sig_ = Signature(session, has_key, started);
        RenderList(fb, width, height);
        return;
    }
    BuildLines(session, has_key, started, width - 2 * kMarginX);
    // New session / new messages: start at the newest question so its answer is read from the top.
    if (session.id != shown_session_ || session.messages.size() != shown_count_) {
        top_line_ = answer_start_line_;
        shown_session_ = session.id;
        shown_count_ = session.messages.size();
    }
    const int per_page = LinesPerPage();
    top_line_ = std::max(0, std::min(top_line_, std::max(0, static_cast<int>(lines_.size()) - per_page)));
    const int page = (top_line_ + per_page - 1) / per_page;  // 0-based; a page starting mid-way counts as the next
    shown_sig_ = Signature(session, has_key, started);

    // Header: session / page counters (the global status bar above already shows the page title).
    DrawRect(fb, width, {kMarginX, body_top + kHeaderHeight, width - 2 * kMarginX, 2}, YELLOW);  // four-colour accent rule
    char counter[96];
    snprintf(counter, sizeof(counter), "会话 %d/%d  第 %d/%d 页", session_index + 1,
             static_cast<int>(std::max<size_t>(1, sessions.size())), page + 1, PageCount());
    DrawText(fb, width, kMarginX, InkCenteredTextTopY(font_, counter, body_top + kHeaderHeight / 2, 0), counter, font_,
             RED);

    // Every row is placed by its vertical centre with the same helper as the header counter (ink-centred on a reference
    // glyph, so all rows share one baseline), from fixed pitch constants.
    const int text_top = body_top + kHeaderHeight + 4;
    const int ref_offset = InkCenteredTextTopY(font_, "国", 0, 0);
    const int shown = std::min(per_page, static_cast<int>(lines_.size()) - top_line_);
    for (int row = 0; row < shown; ++row) {
        const Line& line = lines_[top_line_ + row];
        // Questions and failures in red, hints in the secondary colour, answers in black.
        Color color = text_style.fg;
        if (line.question || line.text.rfind("提问失败", 0) == 0) color = RED;
        else if (line.secondary) color = secondary_style.fg;
        const int center_y = text_top + kLinePitch / 2 + row * kLinePitch;
        DrawText(fb, width, kMarginX, center_y + ref_offset, line.text.c_str(), font_, color);
    }

    FooterBar footer;
    footer.SetBounds(width, height);
    footer.SetText("UP/DN翻页", "长按UP语音提问", "BOOT会话");
    footer.Draw(fb, width, height);
    needs_full_refresh_ = false;
}

void AiChatRenderer::RenderList(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle text_style = theme.Style(ThemeToken::TextPrimary);
    const PaintStyle secondary_style = theme.Style(ThemeToken::TextSecondary);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    auto& svc = ai::ChatService::Get();
    const auto sessions = svc.List();
    const int current = svc.CurrentSessionId();
    const int rows_total = static_cast<int>(sessions.size()) + 1;
    list_sel_ = std::max(0, std::min(list_sel_, rows_total - 1));

    const int body_top = Style::kStatusBarHeight;
    const char* title = "会话列表";
    DrawText(fb, width, kMarginX, InkCenteredTextTopY(font_, title, body_top + kHeaderHeight / 2, 0), title, font_, RED);
    DrawRect(fb, width, {kMarginX, body_top + kHeaderHeight, width - 2 * kMarginX, 2}, YELLOW);

    constexpr int kRowH = 28;
    const int list_top = body_top + kHeaderHeight + 4;
    const int visible = std::max(1, (height - Style::kFooterBarHeight - list_top - 4) / kRowH);
    const int first = std::max(0, std::min(list_sel_ - visible + 1, rows_total - visible));
    const int shown = std::min(visible, rows_total - std::max(0, first));
    for (int row = 0; row < shown; ++row) {
        const int index = std::max(0, first) + row;
        const int y = list_top + row * kRowH;
        const bool selected = index == list_sel_;
        if (selected) {
            DrawStyledRoundRect(fb, width, height, {kMarginX, y, width - 2 * kMarginX, kRowH - 2},
                                Style::kBorderRadiusMD, selected_style);
        }
        const Color fg = selected ? selected_style.fg : text_style.fg;
        const Color hint = selected ? selected_style.fg : secondary_style.fg;
        std::string label;
        std::string tail;
        if (index == 0) {
            label = "+ 新会话";
        } else {
            const ai::SessionInfo& s = sessions[index - 1];
            label = std::string(s.id == current ? "> " : "  ") + std::to_string(index) + ". " +
                    (s.title.empty() ? "（空）" : s.title);
            tail = s.pending ? "回答中" : std::to_string(s.message_count / 2) + "轮";
        }
        const int cy = y + kRowH / 2 - 1;
        DrawText(fb, width, kMarginX + 8, InkCenteredTextTopY(font_, label.c_str(), cy, 0), label.c_str(), font_, fg);
        if (!tail.empty()) {
            DrawText(fb, width, width - kMarginX - 8 - MeasureTextWidth(tail.c_str(), font_),
                     InkCenteredTextTopY(font_, tail.c_str(), cy, 0), tail.c_str(), font_, hint);
        }
    }
    FooterBar footer;
    footer.SetBounds(width, height);
    footer.SetText("UP/DN选择", "BOOT开 长按UP新建", "长按DN删除");
    footer.Draw(fb, width, height);
    needs_full_refresh_ = false;
}

bool AiChatRenderer::HandleInput(const ButtonEvent& event) {
    silent_input_ = false;
    auto& svc = ai::ChatService::Get();
    const bool capturing = backend_ && (backend_->State() == voice::BackendState::Recording ||
                                        backend_->State() == voice::BackendState::Saving);
    const int per_page = LinesPerPage();
    struct Guard {  // service callbacks fired by our own change must not queue a second refresh
        bool& flag;
        explicit Guard(bool& f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
    } guard(in_input_);

    if (list_mode_ && svc.Started()) {  // session list: manage sessions on the device
        if (capturing) return true;
        const auto sessions = svc.List();
        const int rows_total = static_cast<int>(sessions.size()) + 1;
        switch (event.type) {
            case ButtonEvent::kUpClick: list_sel_ = (list_sel_ + rows_total - 1) % rows_total; break;
            case ButtonEvent::kDownClick: list_sel_ = (list_sel_ + 1) % rows_total; break;
            case ButtonEvent::kBootClick:
                if (list_sel_ == 0) {
                    if (svc.CreateSession() < 0) return true;  // full (kMaxSessions): stay in the list
                } else {
                    svc.SelectSession(sessions[list_sel_ - 1].id);
                }
                list_mode_ = false;
                break;
            case ButtonEvent::kUpLongPress:  // shortcut for the "+ 新会话" row
                if (svc.CreateSession() < 0) return true;
                list_mode_ = false;
                break;
            case ButtonEvent::kUpLongRelease:  // the release after that long press: nothing to do, no refresh
                silent_input_ = true;
                return true;
            case ButtonEvent::kDownLongPress:
                if (list_sel_ == 0) return true;
                svc.DeleteSession(sessions[list_sel_ - 1].id);  // no confirmation; the last one is replaced by an empty one
                list_sel_ = std::min(list_sel_, static_cast<int>(svc.List().size()));
                break;
            default: return false;
        }
        needs_full_refresh_ = true;
        return true;
    }

    if (event.type == ButtonEvent::kUpLongPress) {  // hold UP: record a question
        if (!backend_ || !store_ || capturing) return true;
        std::string error;
        const int id = svc.CurrentSessionId();
        if (!svc.VoiceReady(id, &error)) {  // no keys / busy: say so on the panel instead of recording for nothing
            svc.SetSessionError(id, error);
            needs_full_refresh_ = true;
            return true;
        }
        if (backend_->State() == voice::BackendState::Playing) backend_->StopPlayback();
        if (backend_->StartRecording()) {
            recording_for_question_ = true;
            recording_session_ = id;
        }
        silent_input_ = true;
        return true;
    }
    if (event.type == ButtonEvent::kUpLongRelease) {
        if (!recording_for_question_ || !backend_ || backend_->State() != voice::BackendState::Recording) return false;
        backend_->StopRecording();  // OnRecordingSaved() continues from the backend's saving thread
        silent_input_ = true;
        return true;
    }
    if (capturing) {  // swallow other keys while the microphone is busy, without a refresh
        silent_input_ = true;
        return true;
    }

    switch (event.type) {
        case ButtonEvent::kUpClick:
        case ButtonEvent::kDownClick: {
            const int delta = event.type == ButtonEvent::kUpClick ? -1 : 1;
            const int last_top = std::max(0, static_cast<int>(lines_.size()) - per_page);
            top_line_ = std::max(0, std::min(last_top, top_line_ + delta * per_page));
            needs_full_refresh_ = true;
            return true;
        }
        case ButtonEvent::kBootClick: {
            if (!svc.Started()) return true;
            list_mode_ = true;
            list_sel_ = 0;
            const auto sessions = svc.List();
            for (size_t i = 0; i < sessions.size(); ++i)
                if (sessions[i].id == svc.CurrentSessionId()) list_sel_ = static_cast<int>(i) + 1;
            needs_full_refresh_ = true;
            return true;
        }
        case ButtonEvent::kDownLongPress:
            if (svc.Started()) svc.CreateSession();  // no-op at kMaxSessions
            needs_full_refresh_ = true;
            return true;
        default:
            return false;
    }
}

}  // namespace rawdraw
