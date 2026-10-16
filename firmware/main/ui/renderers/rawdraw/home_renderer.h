/**
 * @file home_renderer.h
 * @brief Home page: a list of apps. UP/DOWN select, BOOT opens the selected app.
 *
 * The list comes from ui::GetApps(). Opening an app is delegated through the launch callback so the
 * renderer stays independent of page switching and hardware.
 */

#ifndef RAWDRAW_HOME_RENDERER_H
#define RAWDRAW_HOME_RENDERER_H

#include <functional>

#include "page_renderer.h"
#include "ui/app_registry.h"

namespace rawdraw {

class HomeRenderer : public PageRenderer {
public:
    using LaunchCallback = std::function<void(const ui::AppEntry&)>;

    HomeRenderer();
    ~HomeRenderer() override;

    // PageRenderer interface
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    void SetLaunchCallback(LaunchCallback callback) { launch_cb_ = std::move(callback); }
    int GetSelectedIndex() const { return selected_index_; }
    void SetSelectedIndex(int index) {
        selected_index_ = index;
        ClampWindow();
        needs_full_refresh_ = true;
    }

private:
    static constexpr int kVisibleRows = 5;

    void ClampWindow();

    LaunchCallback launch_cb_;
    int selected_index_ = 0;   // survives Init() so returning Home keeps the last app highlighted
    int first_visible_ = 0;
    const lv_font_t* font_;
    const lv_font_t* title_font_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_HOME_RENDERER_H
