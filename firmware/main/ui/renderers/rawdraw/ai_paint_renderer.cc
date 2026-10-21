#include "ai_paint_renderer.h"

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
constexpr int kLinePitch = 17;
constexpr int kToastHeight = 20;

size_t Utf8Len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}
}  // namespace

AiPaintRenderer::AiPaintRenderer()
    : font_(&SourceHanSansSC_Regular_slim), title_font_(&SourceHanSansSC_Medium_slim) {}

AiPaintRenderer::~AiPaintRenderer() = default;

void AiPaintRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    recording_for_subject_ = false;
    toast_.clear();
    shown_sig_.clear();
    fullscreen_ = false;
}

void AiPaintRenderer::OnLeave() {
    // Unclaim first: the backend may finish saving (and call OnRecordingSaved) before StopRecording() returns.
    const bool was_ours = recording_for_subject_;
    recording_for_subject_ = false;
    if (backend_ && was_ours && backend_->State() == voice::BackendState::Recording) backend_->StopRecording();
}

bool AiPaintRenderer::OnRecordingSaved(const std::string& path) {
    if (!recording_for_subject_) return false;
    recording_for_subject_ = false;
    std::string error;
    auto& svc = ai::paint::PaintService::Get();
    // Transcribes and paints on its own tasks and deletes the file; on failure the file is ours to delete.
    if (!svc.AskVoice(path, &error)) {
        remove(path.c_str());
        svc.SetError(error);
    }
    return true;
}

std::string AiPaintRenderer::Signature(const ai::paint::Status& s) const {
    // While working only "working" counts: the sub-steps (transcribe, generate, download, convert) must not each cost a
    // 15 s panel refresh. The refresh at the start shows the working screen, the one at the end shows the result.
    if (s.working) return "W";
    return std::to_string(static_cast<int>(s.phase)) + "/" + std::to_string(s.revision) + "/" + (s.saved ? "s" : "-") +
           "/" + s.error;
}

bool AiPaintRenderer::NeedsRefreshAfterChange() {
    if (in_input_) return false;  // the key handler already redraws
    auto& svc = ai::paint::PaintService::Get();
    return Signature(svc.GetStatus()) != shown_sig_;
}

void AiPaintRenderer::WrapInto(const std::string& text, int width, std::vector<std::string>* lines) const {
    std::string current;
    int current_w = 0;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\n') {
            lines->push_back(current);
            current.clear();
            current_w = 0;
            ++i;
            continue;
        }
        const size_t n = std::min(Utf8Len(static_cast<unsigned char>(text[i])), text.size() - i);
        const std::string ch = text.substr(i, n);
        i += n;
        const int w = MeasureTextWidth(ch.c_str(), font_);
        if (current_w + w > width && !current.empty()) {
            lines->push_back(current);
            current.clear();
            current_w = 0;
        }
        current += ch;
        current_w += w;
    }
    if (!current.empty() || lines->empty()) lines->push_back(current);
}

void AiPaintRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    width_ = width;
    height_ = height;
    auto& svc = ai::paint::PaintService::Get();
    const ai::paint::Status status = svc.GetStatus();
    shown_sig_ = Signature(status);
    // The picture view needs a picture and nothing in progress; errors keep the previous picture on screen.
    fullscreen_ = status.has_image && !status.working;
    if (fullscreen_) {
        RenderPicture(fb, width, height, status);
    } else {
        RenderText(fb, width, height, status, svc.HasApiKey(), svc.Started());
    }
    needs_full_refresh_ = false;
}

void AiPaintRenderer::RenderPicture(uint8_t* fb, int width, int height, const ai::paint::Status& s) {
    std::vector<uint8_t> image;
    if (!ai::paint::PaintService::Get().CopyImage(&image) || image.size() < ai::paint::kImageBytes) return;
    const int w = std::min(width, ai::paint::kWidth);
    const int h = std::min(height, ai::paint::kHeight);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = image.data() + y * (ai::paint::kWidth / 4);
        for (int x = 0; x < w; ++x) {
            const int code = (row[x >> 2] >> (6 - (x & 3) * 2)) & 0x03;
            set_pixel(fb, width, x, y, static_cast<Color>(code));  // 00 black, 01 white, 10 yellow, 11 red: the Color order
        }
    }

    // A result line (error or "saved") sits just above the hint bar, in red for errors.
    std::string note = toast_;
    bool is_error = false;
    if (!s.error.empty()) {
        note = s.error;
        is_error = true;
    } else if (note.empty()) {
        note = s.saved ? "已保存至相册" : "画: " + s.subject;  // what was painted, shown together with the new picture
    }
    // One line only: cut to the panel width on a character boundary, with an ellipsis.
    const int max_w = width - 2 * kMarginX;
    bool cut = false;
    while (note.size() > 3 && MeasureTextWidth((note + "…").c_str(), font_) > max_w && MeasureTextWidth(note.c_str(), font_) > max_w) {
        do {
            note.pop_back();
        } while (!note.empty() && (static_cast<unsigned char>(note.back()) & 0xC0) == 0x80);
        if (!note.empty() && static_cast<unsigned char>(note.back()) >= 0xC0) note.pop_back();
        cut = true;
    }
    if (cut) note += "…";
    if (!note.empty()) {
        const int y = height - Style::kFooterBarHeight - kToastHeight;
        DrawRect(fb, width, {0, y, width, kToastHeight}, WHITE);
        DrawRect(fb, width, {0, y, width, 1}, BLACK);
        const int tw = MeasureTextWidth(note.c_str(), font_);
        DrawText(fb, width, std::max(kMarginX, (width - tw) / 2),
                 InkCenteredTextTopY(font_, note.c_str(), y + kToastHeight / 2 + 1, 0), note.c_str(), font_,
                 is_error ? RED : BLACK);
    }
    FooterBar footer;
    footer.SetBounds(width, height);
    footer.SetText("UP重画", "DN存相册", "长按UP新画");
    footer.Draw(fb, width, height);
}

void AiPaintRenderer::RenderText(uint8_t* fb, int width, int height, const ai::paint::Status& s, bool has_key,
                                 bool started) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle text_style = theme.Style(ThemeToken::TextPrimary);
    const PaintStyle secondary_style = theme.Style(ThemeToken::TextSecondary);
    const int body_top = Style::kStatusBarHeight;
    DrawStyledRect(fb, width, {0, body_top, width, height - body_top}, bg_style);

    const char* title = "AI 画图";
    DrawText(fb, width, kMarginX, InkCenteredTextTopY(font_, title, body_top + kHeaderHeight / 2, 0), title, font_, RED);
    DrawRect(fb, width, {kMarginX, body_top + kHeaderHeight, width - 2 * kMarginX, 2}, YELLOW);

    struct Line {
        std::string text;
        Color color;
    };
    std::vector<Line> lines;
    auto add = [&](const std::string& text, Color color) {
        std::vector<std::string> wrapped;
        WrapInto(text, width - 2 * kMarginX, &wrapped);
        for (auto& l : wrapped) lines.push_back({l, color});
    };
    if (!started) {
        add("AI 服务未启动", text_style.fg);
        add("请先连接 Wi-Fi。", secondary_style.fg);
    } else if (!has_key) {
        add("还没有设置百炼 API Key", text_style.fg);
        add("请用手机或电脑浏览器打开:", secondary_style.fg);
        add((host_.empty() ? std::string("设备地址") : "http://" + host_) + "/ai", secondary_style.fg);
        add("填入阿里云百炼 API Key。", secondary_style.fg);
    } else if (s.working) {
        // No subject here: while working, Status::subject is still the previous one, and the slow panel is not
        // refreshed again to correct it. What was painted is shown with the picture.
        add("正在绘制，请稍候…", text_style.fg);
        add("一般需要 20 到 60 秒，画好后自动显示。", secondary_style.fg);
    } else {
        add("按住 UP 键，说出想画的内容，", text_style.fg);
        add("松开后开始绘制。", text_style.fg);
        add("例如: 上海外滩的夜景、一只在樱花树下的猫。", secondary_style.fg);
        add("画好后: UP 重新画，DOWN 保存到相册。", secondary_style.fg);
        if (!s.error.empty()) {
            lines.push_back({"", text_style.fg});
            add("失败: " + s.error, RED);
        }
    }
    const int text_top = body_top + kHeaderHeight + 8;
    const int ref_offset = InkCenteredTextTopY(font_, "国", 0, 0);
    const int max_rows = std::max(1, (height - Style::kFooterBarHeight - text_top - 4) / kLinePitch);
    for (int row = 0; row < static_cast<int>(lines.size()) && row < max_rows; ++row) {
        const int center_y = text_top + kLinePitch / 2 + row * kLinePitch;
        DrawText(fb, width, kMarginX, center_y + ref_offset, lines[row].text.c_str(), font_, lines[row].color);
    }
    FooterBar footer;
    footer.SetBounds(width, height);
    footer.SetText("", "长按UP说话", "");
    footer.Draw(fb, width, height);
}

bool AiPaintRenderer::HandleInput(const ButtonEvent& event) {
    silent_input_ = false;
    auto& svc = ai::paint::PaintService::Get();
    const bool capturing = backend_ && (backend_->State() == voice::BackendState::Recording ||
                                        backend_->State() == voice::BackendState::Saving);
    struct Guard {  // service callbacks fired by our own change must not queue a second refresh
        bool& flag;
        explicit Guard(bool& f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
    } guard(in_input_);

    if (event.type == ButtonEvent::kUpLongPress) {  // hold UP: record a subject
        if (!backend_ || !store_ || capturing) return true;
        std::string error;
        if (!svc.VoiceReady(&error)) {
            if (svc.GetStatus().working) {  // already painting: ignore, no refresh
                silent_input_ = true;
                return true;
            }
            svc.SetError(error);
            toast_.clear();
            needs_full_refresh_ = true;
            return true;
        }
        if (backend_->State() == voice::BackendState::Playing) backend_->StopPlayback();
        if (backend_->StartRecording()) recording_for_subject_ = true;
        silent_input_ = true;
        return true;
    }
    if (event.type == ButtonEvent::kUpLongRelease) {
        if (!recording_for_subject_ || !backend_ || backend_->State() != voice::BackendState::Recording) return false;
        backend_->StopRecording();  // OnRecordingSaved() continues from the backend's saving thread
        silent_input_ = true;
        return true;
    }
    if (capturing || svc.GetStatus().working) {  // swallow other keys while the microphone or the painter is busy
        silent_input_ = true;
        return true;
    }

    std::string error;
    switch (event.type) {
        case ButtonEvent::kUpClick:
            toast_.clear();
            if (svc.GetStatus().subject.empty()) {
                silent_input_ = true;  // nothing to redraw yet: the page already says how to start
                return true;
            }
            if (!svc.Regenerate(&error)) svc.SetError(error);
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kDownClick:
            if (!svc.GetStatus().has_image) {
                silent_input_ = true;
                return true;
            }
            if (svc.SaveToAlbum(&error)) toast_ = "已保存至相册";
            else toast_ = error;
            needs_full_refresh_ = true;
            return true;
        default:
            return false;
    }
}

}  // namespace rawdraw
