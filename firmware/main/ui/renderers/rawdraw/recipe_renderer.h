/**
 * @file recipe_renderer.h
 * @brief 菜谱 app: a list of the built-in recipes (common/recipes.h) and a two-page view of each.
 *
 * List: UP/DOWN select (rapid presses are coalesced into one refresh by the UI manager), BOOT opens.
 * Detail (full-screen, no status bar): page 1 = photo, difficulty, calories, intro; page 2 = ingredients and
 * steps. DOWN goes to the next page, and after page 2 on to page 1 of the next recipe; UP goes back the same
 * way. BOOT returns to the list; BOOT long press leaves the app (handled globally by AppShell).
 */

#ifndef RAWDRAW_RECIPE_RENDERER_H
#define RAWDRAW_RECIPE_RENDERER_H

#include <string>
#include <vector>

#include "page_renderer.h"

namespace rawdraw {

class RecipeRenderer : public PageRenderer {
public:
    RecipeRenderer();
    ~RecipeRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    /** True while a recipe is open: the page covers the whole panel, so the manager hides its status bar/frame. */
    bool IsFullscreenMode() const { return detail_; }

    int GetSelectedIndex() const { return selected_index_; }
    int GetPage() const { return page_; }
    bool InDetail() const { return detail_; }

    /** Layout of page 2 for tests: pixel height used by the content of recipe `index` (<= panel height when it fits). */
    static int DetailPageHeight(int index, int* ingredient_lines = nullptr, int* tip_shown = nullptr);
    /** Width in pixels the step text may use. */
    static int StepTextWidth();

private:
    static constexpr int kVisibleRows = 5;

    void ClampWindow();
    void RenderList(uint8_t* fb, int width, int height);
    void RenderCover(uint8_t* fb, int width, int height);
    void RenderSteps(uint8_t* fb, int width, int height);

    bool detail_ = false;
    int page_ = 0;             // 0 = photo/intro, 1 = ingredients/steps
    int selected_index_ = 0;   // survives Init() so returning to the app keeps the last recipe
    int first_visible_ = 0;
    const lv_font_t* font_;
    const lv_font_t* title_font_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_RECIPE_RENDERER_H
