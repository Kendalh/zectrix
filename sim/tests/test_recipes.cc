// Unit test for the 菜谱 app: the built-in data (common/recipes.*) against the real fonts and the panel size, and the
// button logic of RecipeRenderer. Both are compiled unmodified from firmware/main.
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "common/recipes.h"
#include "rawdraw/font_engine.h"
#include "rawdraw/rawdraw.h"
#include "ui/renderers/rawdraw/recipe_renderer.h"

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)
#define CHECK_MSG(cond, ...)                                                     \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d  ", __FILE__, __LINE__);                 \
            fprintf(stderr, __VA_ARGS__);                                        \
            fprintf(stderr, "\n");                                               \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)

using rawdraw::ButtonEvent;
using rawdraw::RecipeRenderer;

namespace {

std::vector<std::string> Chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        const size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

uint32_t CodePoint(const std::string& ch) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(ch.data());
    switch (ch.size()) {
        case 1: return p[0];
        case 2: return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        case 3: return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        default: return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
}

// A missing glyph renders as blank (see the FA_SETTINGS_LINK note in CLAUDE.md), so every character must exist.
void CheckGlyphs(const char* what, const std::string& text, const lv_font_t* font, const char* title) {
    for (const std::string& ch : Chars(text)) {
        lv_font_glyph_dsc_t g;
        CHECK_MSG(lv_font_get_glyph_dsc(font, &g, CodePoint(ch), 0), "%s: no glyph for '%s' in \"%s\" (%s)", title,
                  ch.c_str(), text.c_str(), what);
    }
}

// Greedy wrap like the renderer (closing punctuation may not start a line), only the line count is needed.
int WrappedLines(const std::string& text, const lv_font_t* font, int max_w) {
    int lines = 1;
    std::string cur;
    for (const std::string& ch : Chars(text)) {
        const bool closing = ch == "，" || ch == "。" || ch == "、" || ch == "；" || ch == "：" || ch == "！" || ch == "？";
        if (!cur.empty() && rawdraw::MeasureTextWidth((cur + ch).c_str(), font) > max_w && !closing) {
            ++lines;
            cur.clear();
        }
        cur += ch;
    }
    return lines;
}

void TestData() {
    // Negative control: the glyph lookup really reports characters the font lacks, so the checks below can fail.
    lv_font_glyph_dsc_t probe;
    CHECK(!lv_font_get_glyph_dsc(&SourceHanSansSC_Regular_slim, &probe, 0x1F990, 0));  // U+1F990 shrimp emoji
    CHECK(lv_font_get_glyph_dsc(&SourceHanSansSC_Regular_slim, &probe, CodePoint("虾"), 0));
    const int n = recipes::Count();
    CHECK(n == 10);
    std::set<std::string> ids;
    for (int i = 0; i < n; ++i) {
        const recipes::Recipe& r = recipes::Get(i);
        const char* t = r.title;
        CHECK_MSG(ids.insert(r.id).second, "duplicate id %s", r.id);
        CHECK(r.stars >= 1 && r.stars <= 5);
        CHECK(r.kcal > 0 && r.minutes >= 0);
        CHECK(r.item_count > 0 && r.step_count > 0);
        CHECK(r.photo != nullptr);

        // The photo must use more than one color and not be a flat fill.
        std::set<int> colors;
        for (int k = 0; k < recipes::kPhotoBytes; ++k) {
            for (int s = 0; s < 4; ++s) colors.insert((r.photo[k] >> (s * 2)) & 3);
        }
        CHECK_MSG(colors.size() >= 3, "%s: photo has %zu colors", t, colors.size());

        // Glyphs: list titles and tags use the 24 px Medium font, everything else the 16 px Regular one.
        CheckGlyphs("title", r.title, &SourceHanSansSC_Medium_slim, t);
        CheckGlyphs("tag", r.tag, &SourceHanSansSC_Regular_slim, t);
        CheckGlyphs("serving", r.serving, &SourceHanSansSC_Regular_slim, t);
        CheckGlyphs("intro", r.intro, &SourceHanSansSC_Regular_slim, t);
        CheckGlyphs("tip", r.tip, &SourceHanSansSC_Regular_slim, t);
        for (int k = 0; k < r.item_count; ++k) {
            CheckGlyphs("item", r.items[k].name, &SourceHanSansSC_Regular_slim, t);
            CheckGlyphs("amount", r.items[k].amount, &SourceHanSansSC_Regular_slim, t);
        }
        for (int k = 0; k < r.step_count; ++k) {
            CheckGlyphs("step", r.steps[k], &SourceHanSansSC_Regular_slim, t);
            // Every step is one line: the page-2 budget (300 px) is planned that way.
            CHECK_MSG(WrappedLines(r.steps[k], &SourceHanSansSC_Regular_slim, RecipeRenderer::StepTextWidth()) == 1,
                      "%s: step %d wraps: %s", t, k + 1, r.steps[k]);
        }
        CHECK_MSG(WrappedLines(r.intro, &SourceHanSansSC_Regular_slim, 400 - 16) <= 3, "%s: intro is over 3 lines", t);
        CHECK_MSG(rawdraw::MeasureTextWidth(r.title, &SourceHanSansSC_Medium_slim) <= 400 - 2 * 8, "%s: title too wide", t);
        CHECK_MSG(rawdraw::MeasureTextWidth(r.title, &SourceHanSansSC_Medium_slim) <= 400 - 2 * 14 - 24 - 14 - 120,
                  "%s: title runs into the stars in the list", t);

        int ing_lines = 0, tip_shown = 0;
        const int h = RecipeRenderer::DetailPageHeight(i, &ing_lines, &tip_shown);
        CHECK_MSG(h <= 300, "%s: page 2 needs %d px (ingredients %d lines)", t, h, ing_lines);
        CHECK_MSG(r.tip[0] == 0 || tip_shown, "%s: tip does not fit on page 2", t);
        printf("  %-8s page 2 = %3d px, %d ingredient lines%s\n", t, h, ing_lines, tip_shown ? ", tip" : "");
    }
    // Get() wraps around in both directions.
    CHECK(&recipes::Get(n) == &recipes::Get(0));
    CHECK(&recipes::Get(-1) == &recipes::Get(n - 1));
}

bool Press(RecipeRenderer& r, ButtonEvent::Type type) { return r.HandleInput(ButtonEvent{type}); }

void TestNavigation() {
    const int n = recipes::Count();
    RecipeRenderer r;
    r.Init(400, 300);
    CHECK(!r.InDetail() && !r.IsFullscreenMode() && r.GetSelectedIndex() == 0);

    // List: UP/DOWN select and wrap.
    CHECK(Press(r, ButtonEvent::kDownClick) && r.GetSelectedIndex() == 1);
    CHECK(Press(r, ButtonEvent::kUpClick) && r.GetSelectedIndex() == 0);
    CHECK(Press(r, ButtonEvent::kUpClick) && r.GetSelectedIndex() == n - 1);
    CHECK(Press(r, ButtonEvent::kDownClick) && r.GetSelectedIndex() == 0);
    for (int i = 0; i < 7; ++i) Press(r, ButtonEvent::kDownClick);
    CHECK(r.GetSelectedIndex() == 7);

    // BOOT opens the selected recipe, full screen, page 1.
    CHECK(Press(r, ButtonEvent::kBootClick));
    CHECK(r.InDetail() && r.IsFullscreenMode() && r.GetPage() == 0 && r.GetSelectedIndex() == 7);

    // DOWN: page 2, then the next recipe's page 1. UP goes back the same way.
    CHECK(Press(r, ButtonEvent::kDownClick) && r.GetPage() == 1 && r.GetSelectedIndex() == 7);
    CHECK(Press(r, ButtonEvent::kDownClick) && r.GetPage() == 0 && r.GetSelectedIndex() == 8);
    CHECK(Press(r, ButtonEvent::kUpClick) && r.GetPage() == 1 && r.GetSelectedIndex() == 7);
    CHECK(Press(r, ButtonEvent::kUpClick) && r.GetPage() == 0 && r.GetSelectedIndex() == 7);
    // Past the last recipe it wraps to the first.
    Press(r, ButtonEvent::kBootClick);  // back to the list
    CHECK(!r.InDetail() && r.GetSelectedIndex() == 7);
    for (int i = 0; i < n - 1 - 7; ++i) Press(r, ButtonEvent::kDownClick);
    CHECK(r.GetSelectedIndex() == n - 1);
    Press(r, ButtonEvent::kBootClick);
    Press(r, ButtonEvent::kDownClick);
    CHECK(Press(r, ButtonEvent::kDownClick) && r.GetSelectedIndex() == 0 && r.GetPage() == 0);

    // Long presses are not handled here (BOOT long = Home, in AppShell).
    CHECK(!Press(r, ButtonEvent::kBootLongPress));
    CHECK(!Press(r, ButtonEvent::kUpLongPress));

    // Re-entering the app shows the list again, on the recipe that was open.
    r.Init(400, 300);
    CHECK(!r.InDetail() && r.GetSelectedIndex() == 0);
}

void TestRender() {
    // Every page of every recipe renders without touching memory outside the 400x300 2 bpp buffer.
    for (int i = 0; i < recipes::Count(); ++i) {
        RecipeRenderer r;
        r.Init(400, 300);
        for (int k = 0; k < i; ++k) Press(r, ButtonEvent::kDownClick);
        std::vector<uint8_t> fb(((400 * 2 + 7) >> 3) * 300 + 64, 0xA5);  // guard bytes after the buffer
        r.Render(fb.data(), 400, 300);  // list
        Press(r, ButtonEvent::kBootClick);
        r.Render(fb.data(), 400, 300);  // page 1
        Press(r, ButtonEvent::kDownClick);
        r.Render(fb.data(), 400, 300);  // page 2
        bool guard_ok = true;
        for (size_t k = ((400 * 2 + 7) >> 3) * 300; k < fb.size(); ++k) guard_ok = guard_ok && fb[k] == 0xA5;
        CHECK_MSG(guard_ok, "recipe %d wrote past the framebuffer", i);
    }
}

}  // namespace

int main() {
    TestData();
    TestNavigation();
    TestRender();
    if (g_failed) {
        fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("recipes: all checks passed\n");
    return 0;
}
