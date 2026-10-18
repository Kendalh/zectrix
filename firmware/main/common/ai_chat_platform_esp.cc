/**
 * @file ai_chat_platform_esp.cc
 * @brief ChatPlatform for the device: HTTPS requests (esp_http_client) and NVS key storage. See ai_chat.h.
 *
 * Each request runs in its own short-lived task: the TLS handshake needs a big stack and a
 * non-streaming answer (or a recording upload) can take a minute, which must not block the HTTP server or the UI.
 * ChatService allows one question at a time.
 */

#include "ai_chat_platform_esp.h"

#include "settings.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

namespace {

const char* kTag = "AiChat";
constexpr char kNamespace[] = "aichat";
constexpr size_t kResponseMax = 16 * 1024;
constexpr int kTimeoutMs = 90 * 1000;

struct Job {
    ai::ChatPlatform::HttpRequest request;
    ai::ChatPlatform::DoneCallback done;
};

esp_err_t HttpEvent(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        auto* body = static_cast<std::string*>(evt->user_data);
        if (body->size() + evt->data_len <= kResponseMax) {
            body->append(static_cast<const char*>(evt->data), evt->data_len);
        }  // anything beyond the cap is cut; the JSON then fails to parse and the user sees an error
    }
    return ESP_OK;
}

void RequestTask(void* arg) {
    std::unique_ptr<Job> job(static_cast<Job*>(arg));
    ai::ChatPlatform::HttpRequest& req = job->request;
    std::string response;
    std::string transport_error;
    int status = 0;

    if (!req.body_file.empty()) {  // a recording: read and encoded here, on the big stack, not on the small save task
        if (!ai::BuildFileBody(&req)) transport_error = "录音文件读取失败";
    }

    esp_http_client_handle_t client = nullptr;
    if (transport_error.empty()) {
        esp_http_client_config_t config = {};
        config.url = req.url.c_str();
        config.method = req.method == "GET" ? HTTP_METHOD_GET : HTTP_METHOD_POST;
        config.event_handler = HttpEvent;
        config.user_data = &response;
        config.timeout_ms = kTimeoutMs;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        client = esp_http_client_init(&config);
        if (!client) transport_error = "无法创建连接";
    }
    if (client) {
        for (const auto& h : req.headers) esp_http_client_set_header(client, h.first.c_str(), h.second.c_str());
        if (req.method != "GET") {
            esp_http_client_set_post_field(client, req.body.c_str(), static_cast<int>(req.body.size()));
        }
        const esp_err_t err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "HTTP failed: %s", esp_err_to_name(err));
            transport_error = esp_err_to_name(err);
        }
    }
    ESP_LOGI(kTag, "AI request done: status=%d bytes=%u", status, static_cast<unsigned>(response.size()));
    job->done(status, std::move(response), std::move(transport_error));
    job.reset();
    vTaskDelete(nullptr);
}

class EspChatPlatform : public ai::ChatPlatform {
public:
    void Send(HttpRequest request, DoneCallback done) override {
        auto* job = new Job{std::move(request), std::move(done)};
        if (xTaskCreate(RequestTask, "ai_chat", 10240, job, 3, nullptr) != pdPASS) {
            std::unique_ptr<Job> owned(job);
            if (!owned->request.body_file.empty()) remove(owned->request.body_file.c_str());
            owned->done(0, "", "内存不足");  // ChatService holds no lock while calling the platform, so inline is safe
        }
    }

    // "bl_key": a different slot than the earlier DeepSeek build used, so an old DeepSeek key is never sent to Bailian.
    std::string LoadApiKey() override { return Settings(kNamespace, false).GetString("bl_key", ""); }
    void SaveApiKey(const std::string& key) override { Settings(kNamespace, true).SetString("bl_key", key); }
};

}  // namespace

std::unique_ptr<ai::ChatPlatform> NewEspChatPlatform() { return std::make_unique<EspChatPlatform>(); }
