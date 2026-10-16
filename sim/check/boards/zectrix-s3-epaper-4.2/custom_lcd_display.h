// Minimal stand-in for the board's CustomLcdDisplay, used ONLY by `make check-firmware`.
// It lets the compiler type-check firmware/main/ui/rawdraw_ui_manager.cc on the PC. It is not linked
// into the simulator. Signatures are approximations of the real header: a pass here does not
// replace `idf.py build`.
#pragma once
#include <stdint.h>
#include <mutex>
#include <functional>
#include "esp_timer.h"
#include "freertos/semphr.h"
class CustomLcdDisplay {
public:
    uint8_t* GetFramebuffer() { return nullptr; }
    SemaphoreHandle_t GetMutex() { return nullptr; }
    bool IsRefreshPending() { return false; }
    bool DisplayRaw4ColorImage(const uint8_t*, uint32_t, int, int) { return false; }
    int GetFBWidth() { return 400; }
    int GetFBHeight() { return 300; }
    void SetOnRefreshIdle(std::function<void()>) {}
    void RequestUrgentRefresh() {}
    void RequestUrgentFullRefresh() {}
};
