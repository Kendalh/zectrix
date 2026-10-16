#include "home_renderer.h"

#include <algorithm>

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
constexpr int kIconX = 12;      // inside the row
constexpr int kTitleX = 44;     // inside the row
constexpr int kHintRightPad = 12;
}  // namespace

HomeRenderer::HomeRenderer()
    : font_(&SourceHanSansSC_Regular_slim),
      title_font_(&SourceHanSansSC_Medium_slim) {}

HomeRenderer::~HomeRenderer() = default;

void HomeRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    const int count = static_cast<int>(ui::GetApps().size());
    selected_index_ = count > 0 ? std::min(selected_index_, count - 1) : 0;
    ClampWindow();
}

void HomeRenderer::ClampWindow() {
    const int count = static_cast<int>(ui::GetApps().size());
    if (selected_index_ < first_visible_) first_visible_ = selected_index_;
    if (selected_index_ >= first_visible_ + kVisibleRows) first_visible_ = selected_index_ - kVisibleRows + 1;
    first_visible_ = std::max(0, std::min(first_visible_, std::max(0, count - kVisibleRows)));
}

void HomeRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;

    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle text_style = theme.Style(ThemeToken::TextPrimary);
    const PaintStyle secondary_style = theme.Style(ThemeToken::TextSecondary);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);

    const int body_top = Style::kStatusBarHeight;
    DrawStyledRect(fb, width, {0, body_top, width, height - body_top}, bg_style);

    const auto& apps = ui::GetApps();
    const int count = static_cast<int>(apps.size());
    const int list_top = body_top + 8;
    const int row_w = width - 2 * kListMarginX;

    const int shown = std::min(kVisibleRows, count - first_visible_);
    for (int row = 0; row < shown; ++row) {
        const int index = first_visible_ + row;
        const ui::AppEntry& app = apps[index];
        const bool selected = (index == selected_index_);
        const int y = list_top + row * kRowHeight;
        const int center_y = y + kRowHeight / 2;

        if (selected) {
            DrawStyledRoundRect(fb, width, height, {kListMarginX, y + 3, row_w, kRowHeight - 6},
                                Style::kBorderRadiusMD, selected_style);
        }
        const Color fg = selected ? selected_style.fg : text_style.fg;
        const Color hint_fg = selected ? selected_style.fg : secondary_style.fg;

        const int icon_w = MeasureTextWidth(app.icon, app.icon_font);
        DrawIcon(fb, width, kListMarginX + kIconX + (20 - icon_w) / 2,
                 InkCenteredTextTopY(app.icon_font, app.icon, center_y, 0),
                 app.icon, app.icon_font, fg);
        DrawText(fb, width, kListMarginX + kTitleX,
                 InkCenteredTextTopY(title_font_, app.title, center_y, 0), app.title, title_font_, fg);

        const int hint_w = MeasureTextWidth(app.hint, font_);
        DrawText(fb, width, kListMarginX + row_w - kHintRightPad - hint_w,
                 InkCenteredTextTopY(font_, app.hint, center_y, 0), app.hint, font_, hint_fg);
    }

    // Scroll hints when the list does not fit.
    if (first_visible_ > 0) {
        DrawText(fb, width, width / 2 - 4, list_top - 8, "^", font_, secondary_style.fg);
    }
    if (first_visible_ + kVisibleRows < count) {
        DrawText(fb, width, width / 2 - 4, list_top + kVisibleRows * kRowHeight - 4, "v", font_,
                 secondary_style.fg);
    }

    FooterBar footer;
    footer.SetBounds(width, height);
    footer.SetText("UP/DN选择", nullptr, "BOOT打开");
    footer.Draw(fb, width, height);

    needs_full_refresh_ = false;
}

bool HomeRenderer::HandleInput(const ButtonEvent& event) {
    const int count = static_cast<int>(ui::GetApps().size());
    if (count == 0) return false;

    switch (event.type) {
        case ButtonEvent::kUpClick:
            selected_index_ = (selected_index_ + count - 1) % count;
            ClampWindow();
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kDownClick:
            selected_index_ = (selected_index_ + 1) % count;
            ClampWindow();
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kBootClick:
            if (launch_cb_) launch_cb_(ui::GetApps()[selected_index_]);
            return true;
        default:
            return false;
    }
}

}  // namespace rawdraw
