#include "recipe_renderer.h"

#include <algorithm>
#include <cstdio>

#include "common/recipes.h"
#include "rawdraw/components/footer_bar.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {

constexpr int kPanelW = 400;
constexpr int kPanelH = 300;

// List
constexpr int kRowHeight = 44;
constexpr int kListMarginX = 14;
constexpr int kTitleX = 14;     // inside the row
constexpr int kRightPad = 12;

// Detail
constexpr int kBarH = 24;
constexpr int kMargin = 8;
constexpr int kLineH = 20;      // steps and ingredients
constexpr int kStepGap = 4;
constexpr int kStepTextX = 38;
constexpr int kBadgeW = 21;
constexpr int kBadgeH = 17;
constexpr int kTipH = 22;

// 11x11 star, one string per row.
const char* const kStar[11] = {
    ".....#.....",
    ".....#.....",
    "....###....",
    "....###....",
    "###########",
    ".#########.",
    "..#######..",
    "...#####...",
    "..###.###..",
    "..##...##..",
    ".##.....##.",
};

bool StarPixel(int x, int y) { return x >= 0 && y >= 0 && x < 11 && y < 11 && kStar[y][x] == '#'; }

// A filled star, or only its outline, magnified `scale` times.
void DrawStar(uint8_t* fb, int width, int x, int y, int scale, bool filled, Color color) {
    for (int sy = 0; sy < 11; ++sy) {
        for (int sx = 0; sx < 11; ++sx) {
            if (!StarPixel(sx, sy)) continue;
            const bool edge = !StarPixel(sx - 1, sy) || !StarPixel(sx + 1, sy) || !StarPixel(sx, sy - 1) ||
                              !StarPixel(sx, sy + 1);
            if (filled || edge) FillRect(fb, width, {x + sx * scale, y + sy * scale, scale, scale}, color);
        }
    }
}

void DrawStars(uint8_t* fb, int width, int x, int y, int scale, int count, Color color) {
    for (int i = 0; i < 5; ++i) DrawStar(fb, width, x + i * (11 * scale + scale * 2), y, scale, i < count, color);
}

int StarsWidth(int scale) { return 5 * 11 * scale + 4 * scale * 2; }

size_t Utf8Len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    return 4;
}

bool NoLineStart(const std::string& ch) {
    static const char* const kPunct[] = {"，", "。", "、", "；", "：", "！", "？", "）", "”", "》", "…"};
    for (const char* p : kPunct) {
        if (ch == p) return true;
    }
    return false;
}

// Break at character boundaries so each line is at most max_w wide; closing punctuation never starts a line.
std::vector<std::string> WrapText(const std::string& text, const lv_font_t* font, int max_w) {
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < text.size();) {
        const size_t len = std::min(Utf8Len(static_cast<unsigned char>(text[i])), text.size() - i);
        const std::string ch = text.substr(i, len);
        if (!cur.empty() && MeasureTextWidth((cur + ch).c_str(), font) > max_w && !NoLineStart(ch)) {
            lines.push_back(cur);
            cur.clear();
        }
        cur += ch;
        i += len;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Text with its ink-centered row at `cy`.
void DrawLeft(uint8_t* fb, int width, int x, int cy, const std::string& text, const lv_font_t* font, Color color) {
    DrawText(fb, width, x, InkCenteredTextTopY(font, text.c_str(), cy, 0), text.c_str(), font, color);
}

void DrawRight(uint8_t* fb, int width, int right, int cy, const std::string& text, const lv_font_t* font, Color color) {
    DrawLeft(fb, width, right - MeasureTextWidth(text.c_str(), font), cy, text, font, color);
}

void DrawBar(uint8_t* fb, int width, const std::string& left, const std::string& right, const lv_font_t* font) {
    FillRect(fb, width, {0, 0, width, kBarH}, BLACK);
    DrawLeft(fb, width, kMargin, kBarH / 2, left, font, WHITE);
    DrawRight(fb, width, width - kMargin, kBarH / 2, right, font, YELLOW);
}

void DrawPhoto(uint8_t* fb, int width, int x, int y, const uint8_t* photo) {
    for (int py = 0; py < recipes::kPhotoH; ++py) {
        const uint8_t* row = photo + py * recipes::kPhotoStride;
        for (int px = 0; px < recipes::kPhotoW; ++px) {
            const int code = (row[px >> 2] >> (6 - (px & 3) * 2)) & 0x03;
            set_pixel(fb, width, x + px, y + py, static_cast<Color>(code));  // 00 black, 01 white, 10 yellow, 11 red
        }
    }
}

// A phrase worth noticing in a step: a duration or an amount of liquid. Returns its length in bytes, 0 if none starts here.
size_t HighlightLen(const std::string& s, size_t i) {
    auto digit = [&](size_t k) { return k < s.size() && ((s[k] >= '0' && s[k] <= '9') || s[k] == '-' || s[k] == '.'); };
    if (!digit(i) || (i > 0 && digit(i - 1))) return 0;
    size_t k = i;
    while (digit(k)) ++k;
    for (const char* unit : {"分钟", "秒", "ml", "天"}) {
        const size_t n = std::char_traits<char>::length(unit);
        if (s.compare(k, n, unit) == 0) return k + n - i;
    }
    return 0;
}

// One step line with durations and amounts of liquid in red.
void DrawStepLine(uint8_t* fb, int width, int x, int cy, const std::string& line, const lv_font_t* font) {
    int cx = x;
    for (size_t i = 0; i < line.size();) {
        const size_t hl = HighlightLen(line, i);
        const size_t len = hl ? hl : std::min(Utf8Len(static_cast<unsigned char>(line[i])), line.size() - i);
        const std::string piece = line.substr(i, len);
        DrawLeft(fb, width, cx, cy, piece, font, hl ? RED : BLACK);
        cx += MeasureTextWidth(piece.c_str(), font);
        i += len;
    }
}

// Ingredients packed into lines of whole "name amount" pairs; the pair width includes the gap after it.
struct PackedItem {
    std::string name, amount;
    int name_w, amount_w;
};

std::vector<std::vector<PackedItem>> PackIngredients(const recipes::Recipe& r, int x0, int max_right, const lv_font_t* font,
                                                     const lv_font_t* bold) {
    constexpr int kNameGap = 4;
    constexpr int kItemGap = 14;
    std::vector<std::vector<PackedItem>> lines(1);
    int used = 0;
    for (int i = 0; i < r.item_count; ++i) {
        PackedItem item{r.items[i].name, r.items[i].amount, MeasureTextWidth(r.items[i].name, font),
                        MeasureTextWidth(r.items[i].amount, bold)};
        const int w = item.name_w + kNameGap + item.amount_w + kItemGap;
        if (!lines.back().empty() && x0 + used + w - kItemGap > max_right) {
            lines.emplace_back();
            used = 0;
        }
        lines.back().push_back(item);
        used += w;
    }
    return lines;
}

constexpr int kIngredientX = 48;
constexpr int kIngredientTop = 30;

// Vertical layout of page 2, shared by rendering and the layout test.
struct StepsLayout {
    int ingredient_lines;
    int divider_y;
    int steps_top;
    int end_y;       // first free pixel row below the last step
    bool tip_fits;
};

StepsLayout LayoutSteps(const recipes::Recipe& r, const lv_font_t* font, const lv_font_t* bold) {
    StepsLayout l;
    l.ingredient_lines = static_cast<int>(PackIngredients(r, kIngredientX, kPanelW - kMargin, font, bold).size());
    l.divider_y = kIngredientTop + l.ingredient_lines * kLineH + 2;
    l.steps_top = l.divider_y + 6;
    int y = l.steps_top;
    for (int i = 0; i < r.step_count; ++i) {
        y += static_cast<int>(WrapText(r.steps[i], font, kPanelW - kStepTextX - kMargin).size()) * kLineH + kStepGap;
    }
    l.end_y = y;
    l.tip_fits = r.tip[0] != 0 && y + kTipH <= kPanelH - 2;
    return l;
}

}  // namespace

RecipeRenderer::RecipeRenderer()
    : font_(&SourceHanSansSC_Regular_slim), title_font_(&SourceHanSansSC_Medium_slim) {}

RecipeRenderer::~RecipeRenderer() = default;

int RecipeRenderer::StepTextWidth() { return kPanelW - kStepTextX - kMargin; }

int RecipeRenderer::DetailPageHeight(int index, int* ingredient_lines, int* tip_shown) {
    const StepsLayout l = LayoutSteps(recipes::Get(index), &SourceHanSansSC_Regular_slim, &SourceHanSansSC_Regular_slim);
    if (ingredient_lines) *ingredient_lines = l.ingredient_lines;
    if (tip_shown) *tip_shown = l.tip_fits ? 1 : 0;
    return l.end_y + (l.tip_fits ? kTipH : 0);
}

void RecipeRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    detail_ = false;  // entering the app always shows the list
    page_ = 0;
    selected_index_ = std::min(selected_index_, recipes::Count() - 1);
    ClampWindow();
}

void RecipeRenderer::ClampWindow() {
    const int count = recipes::Count();
    if (selected_index_ < first_visible_) first_visible_ = selected_index_;
    if (selected_index_ >= first_visible_ + kVisibleRows) first_visible_ = selected_index_ - kVisibleRows + 1;
    first_visible_ = std::max(0, std::min(first_visible_, std::max(0, count - kVisibleRows)));
}

void RecipeRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    if (!detail_) {
        RenderList(fb, width, height);
    } else if (page_ == 0) {
        RenderCover(fb, width, height);
    } else {
        RenderSteps(fb, width, height);
    }
    needs_full_refresh_ = false;
}

void RecipeRenderer::RenderList(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle text_style = theme.Style(ThemeToken::TextPrimary);
    const PaintStyle secondary_style = theme.Style(ThemeToken::TextSecondary);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);

    const int body_top = Style::kStatusBarHeight;
    DrawStyledRect(fb, width, {0, body_top, width, height - body_top}, bg_style);

    const int count = recipes::Count();
    const int list_top = body_top + 8;
    const int row_w = width - 2 * kListMarginX;
    const int shown = std::min(kVisibleRows, count - first_visible_);
    for (int row = 0; row < shown; ++row) {
        const int index = first_visible_ + row;
        const recipes::Recipe& r = recipes::Get(index);
        const bool selected = (index == selected_index_);
        const int y = list_top + row * kRowHeight;
        const int center_y = y + kRowHeight / 2;
        if (selected) {
            DrawStyledRoundRect(fb, width, height, {kListMarginX, y + 3, row_w, kRowHeight - 6}, Style::kBorderRadiusMD,
                                selected_style);
        }
        const Color fg = selected ? selected_style.fg : text_style.fg;
        const Color hint_fg = selected ? selected_style.fg : secondary_style.fg;
        DrawLeft(fb, width, kListMarginX + kTitleX, center_y, r.title, title_font_, fg);
        // Difficulty as stars (red on the white rows, in the row's own color on the highlighted one) and calories.
        const int right = kListMarginX + row_w - kRightPad;
        const std::string kcal = std::to_string(r.kcal) + "大卡";
        DrawRight(fb, width, right, center_y, kcal, font_, hint_fg);
        const int stars_x = right - MeasureTextWidth(kcal.c_str(), font_) - 10 - StarsWidth(1);
        DrawStars(fb, width, stars_x, center_y - 5, 1, r.stars, selected ? fg : RED);
    }
    if (first_visible_ > 0) {
        DrawText(fb, width, width / 2 - 4, list_top - 8, "^", font_, secondary_style.fg);
    }
    if (first_visible_ + kVisibleRows < count) {
        DrawText(fb, width, width / 2 - 4, list_top + kVisibleRows * kRowHeight - 4, "v", font_, secondary_style.fg);
    }

    FooterBar footer;
    footer.SetBounds(width, height);
    footer.SetText("UP/DN选择(可连按)", nullptr, "BOOT查看");
    footer.Draw(fb, width, height);
}

void RecipeRenderer::RenderCover(uint8_t* fb, int width, int height) {
    const recipes::Recipe& r = recipes::Get(selected_index_);
    Clear(fb, width, height);
    DrawBar(fb, width, std::string("菜谱 · ") + r.tag, "1/2", font_);

    DrawLeft(fb, width, kMargin, 40, r.title, title_font_, BLACK);

    const int px = kMargin, py = 62;
    DrawPhoto(fb, width, px, py, r.photo);
    DrawRectBorder(fb, width, {px - 1, py - 1, recipes::kPhotoW + 2, recipes::kPhotoH + 2}, 1, BLACK);

    const int x = 200;
    DrawLeft(fb, width, x, py + 6, "难度", font_, BLACK);
    DrawStars(fb, width, x, py + 20, 2, r.stars, RED);
    DrawLeft(fb, width, x, py + 58, "卡路里", font_, BLACK);
    const std::string kcal = std::to_string(r.kcal);
    DrawLeft(fb, width, x, py + 80, kcal, title_font_, RED);
    DrawLeft(fb, width, x + MeasureTextWidth(kcal.c_str(), title_font_) + 4, py + 84, "大卡", font_, BLACK);
    if (r.minutes > 0) {
        DrawLeft(fb, width, x, py + 118, "用时 约 " + std::to_string(r.minutes) + " 分钟", font_, BLACK);
    }

    // Intro: at most 3 lines, clear of the hint bar.
    const auto lines = WrapText(r.intro, font_, width - 2 * kMargin);
    for (int i = 0; i < static_cast<int>(lines.size()) && i < 3; ++i) {
        DrawLeft(fb, width, kMargin, 218 + 9 + i * 18, lines[i], font_, BLACK);
    }

    FooterBar footer;
    footer.SetBounds(width, height);
    footer.SetText("UP/DN翻页(可连按)", nullptr, "BOOT返回");
    footer.Draw(fb, width, height);
}

void RecipeRenderer::RenderSteps(uint8_t* fb, int width, int height) {
    const recipes::Recipe& r = recipes::Get(selected_index_);
    Clear(fb, width, height);
    DrawBar(fb, width, std::string("材料与做法 · ") + r.serving, "2/2", font_);

    // Ingredients: whole pairs per line, amounts in red.
    FillRect(fb, width, {kMargin, kIngredientTop - 1, 34, 20}, RED);
    DrawLeft(fb, width, kMargin + 1, kIngredientTop + 9, "材料", font_, WHITE);
    const auto packed = PackIngredients(r, kIngredientX, width - kMargin, font_, font_);
    for (size_t i = 0; i < packed.size(); ++i) {
        int cx = kIngredientX;
        const int cy = kIngredientTop + static_cast<int>(i) * kLineH + 9;
        for (const PackedItem& item : packed[i]) {
            DrawLeft(fb, width, cx, cy, item.name, font_, BLACK);
            cx += item.name_w + 4;
            DrawLeft(fb, width, cx, cy, item.amount, font_, RED);
            cx += item.amount_w + 14;
        }
    }

    const StepsLayout l = LayoutSteps(r, font_, font_);
    FillRect(fb, width, {kMargin, l.divider_y, width - 2 * kMargin, 2}, YELLOW);

    int y = l.steps_top;
    for (int i = 0; i < r.step_count; ++i) {
        const std::string num = std::to_string(i + 1);
        FillRect(fb, width, {kMargin, y + 1, kBadgeW, kBadgeH}, RED);
        DrawLeft(fb, width, kMargin + (kBadgeW - MeasureTextWidth(num.c_str(), font_)) / 2, y + 1 + kBadgeH / 2, num,
                 font_, WHITE);
        for (const std::string& line : WrapText(r.steps[i], font_, width - kStepTextX - kMargin)) {
            DrawStepLine(fb, width, kStepTextX, y + kLineH / 2, line, font_);
            y += kLineH;
        }
        y += kStepGap;
    }

    if (l.tip_fits) {
        FillRect(fb, width, {kMargin, y, width - 2 * kMargin, kTipH - 2}, YELLOW);
        DrawLeft(fb, width, kMargin + 6, y + (kTipH - 2) / 2, std::string("小贴士 ") + r.tip, font_, BLACK);
    }
}

bool RecipeRenderer::HandleInput(const ButtonEvent& event) {
    const int count = recipes::Count();
    if (count == 0) return false;
    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (!detail_) {
                selected_index_ = (selected_index_ + count - 1) % count;
                ClampWindow();
            } else if (page_ == 1) {
                page_ = 0;
            } else {  // before the first page: the previous recipe's last page
                selected_index_ = (selected_index_ + count - 1) % count;
                ClampWindow();
                page_ = 1;
            }
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kDownClick:
            if (!detail_) {
                selected_index_ = (selected_index_ + 1) % count;
                ClampWindow();
            } else if (page_ == 0) {
                page_ = 1;
            } else {  // after the last page: the next recipe's first page
                selected_index_ = (selected_index_ + 1) % count;
                ClampWindow();
                page_ = 0;
            }
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kBootClick:
            if (detail_) {
                detail_ = false;
                page_ = 0;
            } else {
                detail_ = true;
                page_ = 0;
            }
            needs_full_refresh_ = true;
            return true;
        default:
            return false;
    }
}

}  // namespace rawdraw
