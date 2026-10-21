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

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

const char* kTag = "AiChat";
constexpr char kNamespace[] = "aichat";
constexpr int kTimeoutMs = 90 * 1000;

struct Job {
    ai::ChatPlatform::HttpRequest request;
    ai::ChatPlatform::DoneCallback done;
};

struct Sink {
    std::string* body;
    size_t max;
};

// mbedtls_x509_crt_verify() flag bits (MBEDTLS_X509_BADCERT_*), as words for the log.
std::string TlsFlagsForLog(int flags) {
    static const struct { int bit; const char* name; } kBits[] = {
        {0x01, "EXPIRED"}, {0x02, "REVOKED"}, {0x04, "CN_MISMATCH"}, {0x08, "NOT_TRUSTED"}, {0x40, "MISSING"},
        {0x100, "OTHER"}, {0x200, "FUTURE"}, {0x400, "KEY_USAGE"}, {0x800, "EXT_KEY_USAGE"}, {0x2000, "BAD_MD"},
        {0x4000, "BAD_PK"}, {0x8000, "BAD_KEY"}};
    std::string out;
    for (const auto& b : kBits) {
        if (flags & b.bit) out += std::string(out.empty() ? "" : "|") + b.name;
    }
    return out.empty() ? "-" : out;
}

// Host + path only: the query carries the AccessKeyId and signature, which must not reach the log.
std::string UrlForLog(const std::string& url) {
    const size_t q = url.find('?');
    return q == std::string::npos ? url : url.substr(0, q) + "?<" + std::to_string(url.size() - q - 1) + " B query>";
}

esp_err_t HttpEvent(esp_http_client_event_t* evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR: ESP_LOGW(kTag, "http event: ERROR"); break;
        case HTTP_EVENT_ON_CONNECTED: ESP_LOGI(kTag, "http event: connected"); break;
        case HTTP_EVENT_ON_HEADER: ESP_LOGD(kTag, "http header: %s: %s", evt->header_key, evt->header_value); break;
        case HTTP_EVENT_DISCONNECTED: ESP_LOGI(kTag, "http event: disconnected"); break;
        default: break;
    }
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        auto* sink = static_cast<Sink*>(evt->user_data);
        if (sink->body->size() + evt->data_len <= sink->max) {
            sink->body->append(static_cast<const char*>(evt->data), evt->data_len);
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
    Sink sink{&response, req.max_response};

    if (!req.body_file.empty()) {  // a recording: read and encoded here, on the big stack, not on the small save task
        if (!ai::BuildFileBody(&req)) transport_error = "录音文件读取失败";
    }

    // One attempt; `ca_pem` null = the ESP-IDF certificate bundle, else only that certificate is trusted.
    auto attempt = [&](const char* ca_pem) -> esp_err_t {
        esp_http_client_config_t config = {};
        config.url = req.url.c_str();
        config.method = req.method == "GET" ? HTTP_METHOD_GET : HTTP_METHOD_POST;
        config.event_handler = HttpEvent;
        config.user_data = &sink;
        config.timeout_ms = kTimeoutMs;
        config.buffer_size_tx = 2048;  // the image download URL is a long pre-signed one: the default 512 B request buffer is too small
        if (ca_pem) config.cert_pem = ca_pem;
        else config.crt_bundle_attach = esp_crt_bundle_attach;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            transport_error = "无法创建连接";
            return ESP_FAIL;
        }
        for (const auto& h : req.headers) esp_http_client_set_header(client, h.first.c_str(), h.second.c_str());
        if (req.method != "GET") {
            esp_http_client_set_post_field(client, req.body.c_str(), static_cast<int>(req.body.size()));
        }
        ESP_LOGI(kTag, "request: %s %s (url %u B, body %u B, trust=%s) heap=%u internal=%u max_block=%u",
                 req.method.c_str(), UrlForLog(req.url).c_str(), static_cast<unsigned>(req.url.size()),
                 static_cast<unsigned>(req.body.size()), ca_pem ? "pinned" : "bundle",
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        const int64_t content_length = esp_http_client_get_content_length(client);
        const int sys_errno = esp_http_client_get_errno(client);
        int tls_code = 0, tls_flags = 0;
        esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
        esp_http_client_cleanup(client);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "TLS: code=-0x%x flags=0x%x (%s) device_time=%lld", -tls_code, static_cast<unsigned>(tls_flags),
                     TlsFlagsForLog(tls_flags).c_str(), static_cast<long long>(time(nullptr)));
            ESP_LOGW(kTag, "HTTP failed: %s (0x%x) after %d ms, status=%d content_length=%lld errno=%d (%s) got=%u B",
                     esp_err_to_name(err), static_cast<unsigned>(err),
                     static_cast<int>((esp_timer_get_time() - t0) / 1000), status,
                     static_cast<long long>(content_length), sys_errno, strerror(sys_errno),
                     static_cast<unsigned>(response.size()));
        }
        return err;
    };
    if (transport_error.empty()) {
        esp_err_t err = attempt(nullptr);
        if (err == ESP_ERR_HTTP_CONNECT && req.fallback_ca_pem) {
            ESP_LOGW(kTag, "handshake failed with the certificate bundle, retrying with the pinned CA");
            response.clear();
            err = attempt(req.fallback_ca_pem);
        }
        if (err != ESP_OK && transport_error.empty()) transport_error = esp_err_to_name(err);
    }
    ESP_LOGI(kTag, "AI request done: status=%d bytes=%u", status, static_cast<unsigned>(response.size()));
    if (status >= 400 || (status != 0 && status < 200)) {
        ESP_LOGW(kTag, "HTTP %d body: %.200s", status, response.c_str());
    }
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
