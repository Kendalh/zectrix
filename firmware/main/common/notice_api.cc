#include "common/notice_api.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "settings.h"

#include <esp_timer.h>

#include <algorithm>
#include <atomic>

namespace {

constexpr char kTag[] = "Notice";
constexpr size_t kResponseMax = 1024;  // one line is all we show
constexpr char kNamespace[] = "notice";

NoticeCallback s_callback;
std::atomic<int> s_interval_min{30};
std::atomic<bool> s_loaded{false};
std::atomic<bool> s_entry{false};         // the Calendar page was entered: fetch if the last fetch is stale
std::atomic<int64_t> s_fetched_ms{-1};    // when the last successful fetch finished; -1 = none / invalidated
TaskHandle_t s_task = nullptr;

esp_err_t HttpEvent(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        auto* body = static_cast<std::string*>(evt->user_data);
        const size_t room = kResponseMax > body->size() ? kResponseMax - body->size() : 0;
        body->append(static_cast<const char*>(evt->data), std::min<size_t>(evt->data_len, room));
    }
    return ESP_OK;
}

bool Fetch(std::string* body) {
    esp_http_client_config_t config = {};
    config.url = kNoticeUrl;
    config.method = HTTP_METHOD_GET;
    config.event_handler = HttpEvent;
    config.user_data = body;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(kTag, "fetch failed: %s, status %d", esp_err_to_name(err), status);
        return false;
    }
    return true;
}

bool ValidInterval(int minutes) { return minutes == 0 || minutes == 10 || minutes == 30 || minutes == 60; }

void LoadInterval() {
    if (s_loaded.exchange(true)) return;
    Settings nvs(kNamespace, false);
    const int saved = nvs.GetInt("interval", 30);
    s_interval_min = ValidInterval(saved) ? saved : 30;
}

// The task is idle until asked: nothing is fetched at boot, when Wi-Fi connects, or on a timer. The notice is only shown in the
// calendar app, so the app asks when it is entered (notice_api_refresh_if_stale). The interval is how long a fetched notice
// stays valid: entering the app again before that does not hit the server.
void NoticeTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const bool entry = s_entry.exchange(false);
        const int interval = s_interval_min;
        if (interval == 0) {  // "never fetch": no request; clear the notice once when the setting is saved
            if (!entry && s_callback) s_callback(std::string());
            continue;
        }
        if (!entry) continue;  // the setting changed to another interval: only the validity changed, see set_interval
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const int64_t fetched = s_fetched_ms;
        if (fetched >= 0 && now_ms - fetched < static_cast<int64_t>(interval) * 60 * 1000) continue;  // the page shows it
        std::string body;
        const bool ok = Fetch(&body);
        if (ok) s_fetched_ms = esp_timer_get_time() / 1000;
        if (s_callback) s_callback(ok ? body : std::string());
    }
}

}  // namespace

int notice_api_get_interval() {
    LoadInterval();
    return s_interval_min;
}

bool notice_api_set_interval(int minutes) {
    if (!ValidInterval(minutes)) return false;
    LoadInterval();
    {
        Settings nvs(kNamespace, true);
        nvs.SetInt("interval", minutes);
    }
    s_interval_min = minutes;
    s_fetched_ms = -1;  // the next time the calendar is entered it fetches, whatever the old interval said
    if (s_task) xTaskNotifyGive(s_task);  // only matters for 0: the notice is cleared; nothing is fetched here
    return true;
}

bool notice_api_refresh_if_stale() {
    if (!s_task) return false;
    s_entry = true;
    xTaskNotifyGive(s_task);
    return true;
}

void notice_api_start(NoticeCallback callback) {
    if (s_task) return;
    LoadInterval();
    s_callback = std::move(callback);
    if (xTaskCreate(NoticeTask, "notice", 6144, nullptr, 2, &s_task) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create notice task");
        s_task = nullptr;
    }
}
