#include "fake/token_board_platform_fake.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>

#include "fake/ai_chat_platform_fake.h"

namespace {

bool FakeMode() {
    const char* v = getenv("SIM_BOARD_FAKE");
    return v && *v && *v != '0';
}

std::string Row(const char* instance_id, const char* detail, double amount) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ProductCode\":\"sfm\",\"ProductName\":\"大模型服务平台百炼\",\"ProductDetail\":\"%s\","
             "\"InstanceID\":\"%s\",\"PretaxAmount\":%g}",
             detail, instance_id, amount);
    return buf;
}

std::string Bill(const std::string& rows) {
    return "{\"Code\":\"Success\",\"Message\":\"Successful!\",\"Success\":true,\"Data\":{\"Items\":{\"Item\":[" + rows +
           "]}}}";
}

std::string QueryParam(const std::string& url, const std::string& key) {
    const size_t pos = url.find(key + "=");
    if (pos == std::string::npos) return "";
    const size_t start = pos + key.size() + 1;
    return url.substr(start, url.find('&', start) - start);
}

std::string Canned(const std::string& url, int balance_calls) {
    if (url.find("api.deepseek.com/user/balance") != std::string::npos) {
        char buf[300];
        snprintf(buf, sizeof(buf),
                 "{\"is_available\":true,\"balance_infos\":[{\"currency\":\"CNY\",\"total_balance\":\"%.2f\","
                 "\"granted_balance\":\"10.00\",\"topped_up_balance\":\"%.2f\"}]}",
                 110.0 - 0.05 * balance_calls, 100.0 - 0.05 * balance_calls);
        return buf;
    }
    if (url.find("Action=QueryAccountBalance") != std::string::npos) {
        const double balance = 1234.50 - 0.05 * balance_calls;  // the real API formats with a thousands separator
        const int thousands = static_cast<int>(balance) / 1000;
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "{\"Code\":\"200\",\"Message\":\"successful\",\"Success\":true,\"Data\":{\"AvailableAmount\":\"%d,%06.2f\","
                 "\"Currency\":\"CNY\"}}",
                 thousands, balance - thousands * 1000);
        return buf;
    }
    const board::DateParts t = board::BeijingParts(time(nullptr));
    char today[16];
    snprintf(today, sizeof(today), "%04d-%02d-%02d", t.year, t.month, t.day);
    if (QueryParam(url, "BillingDate") == today) {  // today's daily bill lags: only the first rows are in
        return Bill(Row("1;k;qwen-flash;context_0-128k_input_token;;0", "百炼大模型推理", 0.0156) + "," +
                    Row("1;k;qwen-flash;context_0-128k_output_token;;0", "百炼大模型推理", 0.0062));
    }
    return Bill(Row("1;k;qwen-image-2.0-pro;images;;0", "百炼大模型推理", 16.0) + "," +
                Row("1;k;wan2.2-t2i-flash;images;;0", "百炼大模型推理", 0.14) + "," +
                Row("1;k;qwen3-asr-flash;duration;;0", "百炼大模型推理", 0.02046) + "," +
                Row("cn-beijing;sfm_plugin;;web-search-turbo", "阿里云百炼自研工具", 0.018) + "," +
                Row("1;k;qwen-flash;context_0-128k_input_token;;0", "百炼大模型推理", 0.0053454) + "," +
                Row("1;k;qwen-flash;context_0-128k_output_token;;0", "百炼大模型推理", 0.004083));
}

class SimBoardPlatform : public board::Platform {
public:
    SimBoardPlatform() : http_(NewSimChatPlatform()) {}

    void Send(ai::ChatPlatform::HttpRequest request, ai::ChatPlatform::DoneCallback done) override {
        if (!FakeMode()) {
            http_->Send(std::move(request), std::move(done));
            return;
        }
        const bool is_balance = request.url.find("Action=QueryAccountBalance") != std::string::npos ||
                                request.url.find("/user/balance") != std::string::npos;
        const int calls = is_balance ? ++balance_calls_ : balance_calls_;
        std::thread([url = request.url, calls, done = std::move(done)]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            done(200, Canned(url, calls), "");
        }).detach();
    }

    std::string Load(const std::string& key) override {
        if (key == "ak_id") {
            const char* env = getenv("ALIBABA_CLOUD_ACCESS_KEY_ID");
            if (env && !store_.count(key)) return env;
        } else if (key == "ak_secret") {
            const char* env = getenv("ALIBABA_CLOUD_ACCESS_KEY_SECRET");
            if (env && !store_.count(key)) return env;
        } else if (key == "ds_key") {
            const char* env = getenv("DEEPSEEK_API_KEY");
            if (env && !store_.count(key)) return env;
        }
        if (FakeMode() && (key == "ak_id" || key == "ak_secret" || key == "ds_key") && !store_.count(key)) return "sim-fake";
        auto it = store_.find(key);
        return it == store_.end() ? "" : it->second;
    }
    void Save(const std::string& key, const std::string& value) override { store_[key] = value; }

private:
    std::unique_ptr<ai::ChatPlatform> http_;
    std::map<std::string, std::string> store_;
    int balance_calls_ = 0;
};

}  // namespace

std::unique_ptr<board::Platform> NewSimBoardPlatform() { return std::make_unique<SimBoardPlatform>(); }
