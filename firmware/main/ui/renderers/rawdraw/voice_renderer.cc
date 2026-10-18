#include "voice_renderer.h"

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
constexpr int kRowHeight = 44;
constexpr int kListMarginX = 14;
constexpr int kTitleX = 14;
constexpr int kHintRightPad = 12;

std::string FormatDuration(int ms) {
    const int s = (ms + 500) / 1000;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}
}  // namespace

VoiceRenderer::VoiceRenderer()
    : font_(&SourceHanSansSC_Regular_slim),
      title_font_(&SourceHanSansSC_Medium_slim) {}

VoiceRenderer::~VoiceRenderer() = default;

void VoiceRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    selected_index_ = 0;
    first_visible_ = 0;
    select_newest_ = false;
    playing_path_.clear();
}

void VoiceRenderer::OnLeave() {
    if (!backend_) return;
    switch (backend_->State()) {
        case voice::BackendState::Recording: backend_->StopRecording(); break;
        case voice::BackendState::Playing: backend_->StopPlayback(); break;
        default: break;
    }
    playing_path_.clear();
}

void VoiceRenderer::ClampWindow(int count) {
    selected_index_ = count > 0 ? std::max(0, std::min(selected_index_, count - 1)) : 0;
    if (selected_index_ < first_visible_) first_visible_ = selected_index_;
    if (selected_index_ >= first_visible_ + kVisibleRows) first_visible_ = selected_index_ - kVisibleRows + 1;
    first_visible_ = std::max(0, std::min(first_visible_, std::max(0, count - kVisibleRows)));
}

void VoiceRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;

    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle text_style = theme.Style(ThemeToken::TextPrimary);
    const PaintStyle secondary_style = theme.Style(ThemeToken::TextSecondary);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);

    const int body_top = Style::kStatusBarHeight;
    DrawStyledRect(fb, width, {0, body_top, width, height - body_top}, bg_style);

    const voice::BackendState state = backend_ ? backend_->State() : voice::BackendState::Idle;
    const bool playing = state == voice::BackendState::Playing;
    if (!playing) playing_path_.clear();

    FooterBar footer;
    footer.SetBounds(width, height);

    if (state == voice::BackendState::Recording || state == voice::BackendState::Saving) {
        const char* msg = state == voice::BackendState::Recording ? "录音中…" : "保存中…";
        const char* sub = state == voice::BackendState::Recording ? "松开 UP 结束录音" : "请稍候";
        const int cy = body_top + (height - body_top) / 2;
        DrawText(fb, width, (width - MeasureTextWidth(msg, title_font_)) / 2,
                 InkCenteredTextTopY(title_font_, msg, cy - 16, 0), msg, title_font_, text_style.fg);
        DrawText(fb, width, (width - MeasureTextWidth(sub, font_)) / 2,
                 InkCenteredTextTopY(font_, sub, cy + 20, 0), sub, font_, secondary_style.fg);
        footer.SetText(nullptr, nullptr, nullptr);
        footer.Draw(fb, width, height);
        needs_full_refresh_ = false;
        return;
    }

    std::vector<voice::Recording> items;
    if (store_) items = store_->List();
    const int count = static_cast<int>(items.size());
    if (select_newest_) {
        selected_index_ = 0;
        first_visible_ = 0;
        select_newest_ = false;
    }
    ClampWindow(count);

    const int list_top = body_top + 8;
    const int row_w = width - 2 * kListMarginX;

    if (count == 0) {
        const char* msg = "暂无录音";
        const char* sub = "长按 UP 开始录音";
        const int cy = body_top + (height - body_top) / 2;
        DrawText(fb, width, (width - MeasureTextWidth(msg, title_font_)) / 2,
                 InkCenteredTextTopY(title_font_, msg, cy - 16, 0), msg, title_font_, text_style.fg);
        DrawText(fb, width, (width - MeasureTextWidth(sub, font_)) / 2,
                 InkCenteredTextTopY(font_, sub, cy + 20, 0), sub, font_, secondary_style.fg);
    }

    const int shown = std::min(kVisibleRows, count - first_visible_);
    for (int row = 0; row < shown; ++row) {
        const int index = first_visible_ + row;
        const voice::Recording& rec = items[index];
        const bool selected = (index == selected_index_);
        const int y = list_top + row * kRowHeight;
        const int center_y = y + kRowHeight / 2;

        if (selected) {
            DrawStyledRoundRect(fb, width, height, {kListMarginX, y + 3, row_w, kRowHeight - 6},
                                Style::kBorderRadiusMD, selected_style);
        }
        const Color fg = selected ? selected_style.fg : text_style.fg;
        const Color hint_fg = selected ? selected_style.fg : secondary_style.fg;

        char title[32];
        snprintf(title, sizeof(title), "录音 %d", rec.number);
        DrawText(fb, width, kListMarginX + kTitleX,
                 InkCenteredTextTopY(title_font_, title, center_y, 0), title, title_font_, fg);

        const bool is_playing = playing && rec.path == playing_path_;
        const std::string hint = is_playing ? "播放中" : FormatDuration(rec.duration_ms);
        DrawText(fb, width, kListMarginX + row_w - kHintRightPad - MeasureTextWidth(hint.c_str(), font_),
                 InkCenteredTextTopY(font_, hint.c_str(), center_y, 0), hint.c_str(), font_, hint_fg);
    }

    if (first_visible_ > 0) {
        DrawText(fb, width, width / 2 - 4, list_top - 8, "^", font_, secondary_style.fg);
    }
    if (first_visible_ + kVisibleRows < count) {
        DrawText(fb, width, width / 2 - 4, list_top + kVisibleRows * kRowHeight - 4, "v", font_,
                 secondary_style.fg);
    }

    std::string err = backend_ ? backend_->LastError() : "";
    if (!err.empty()) {
        footer.SetText(err.c_str(), nullptr, nullptr);
    } else {
        footer.SetText("长按UP录音", count > 0 ? "长按DN删除" : nullptr, "BOOT播放/停止");
    }
    footer.Draw(fb, width, height);
    needs_full_refresh_ = false;
}

bool VoiceRenderer::HandleInput(const ButtonEvent& event) {
    silent_input_ = false;
    if (!backend_ || !store_) return false;
    const voice::BackendState state = backend_->State();
    const bool busy_capture = state == voice::BackendState::Recording || state == voice::BackendState::Saving;

    switch (event.type) {
        case ButtonEvent::kUpLongPress:
            if (busy_capture) return true;
            if (state == voice::BackendState::Playing) backend_->StopPlayback();
            backend_->StartRecording();
            silent_input_ = true;
            return true;
        case ButtonEvent::kUpLongRelease:
            if (state != voice::BackendState::Recording) return false;
            backend_->StopRecording();
            select_newest_ = true;
            silent_input_ = true;
            return true;
        default:
            break;
    }
    if (busy_capture) {  // swallow other keys while capturing, without a refresh
        silent_input_ = true;
        return true;
    }

    const int count = static_cast<int>(store_->List().size());
    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (count == 0) return true;
            selected_index_ = (selected_index_ + count - 1) % count;
            ClampWindow(count);
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kDownClick:
            if (count == 0) return true;
            selected_index_ = (selected_index_ + 1) % count;
            ClampWindow(count);
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kDownLongPress: {  // delete the selected recording
            const auto items = store_->List();
            if (selected_index_ >= static_cast<int>(items.size())) return true;
            const std::string path = items[selected_index_].path;
            if (state == voice::BackendState::Playing && path == playing_path_) {
                backend_->StopPlayback();
                playing_path_.clear();
            }
            store_->Remove(path);
            ClampWindow(count - 1);  // the next recording slides into the same row; the last one moves up
            needs_full_refresh_ = true;
            return true;
        }
        case ButtonEvent::kBootClick: {
            if (state == voice::BackendState::Playing) {
                backend_->StopPlayback();
                playing_path_.clear();
            } else {
                const auto items = store_->List();
                if (selected_index_ < static_cast<int>(items.size()) &&
                    backend_->StartPlayback(items[selected_index_].path)) {
                    playing_path_ = items[selected_index_].path;
                }
            }
            silent_input_ = true;  // just play; the panel is not redrawn for it
            return true;
        }
        default:
            return false;
    }
}

}  // namespace rawdraw
