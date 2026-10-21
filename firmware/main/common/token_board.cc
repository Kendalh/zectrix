#include "token_board.h"

#include <cJSON.h>
#include <esp_log.h>

#include "token_board_ca.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace board {

namespace {

constexpr char kHost[] = "business.aliyuncs.com";  // China site; the international site is business.ap-southeast-1.aliyuncs.com
constexpr char kVersion[] = "2017-12-14";
constexpr size_t kBillMaxResponse = 48 * 1024;      // one bill row is ~1.1 KB
constexpr int kBillMaxResults = 50;
constexpr time_t kMinValidTime = 1700000000;        // anything earlier means the clock was never synced

uint32_t Rol(uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }

std::string ErrorText(const std::string& code, const std::string& message) {
    if (code.rfind("InvalidAccessKeyId", 0) == 0) return "AccessKey ID 无效";
    if (code.rfind("SignatureDoesNotMatch", 0) == 0) return "AccessKey Secret 不对";
    if (code.rfind("Forbidden", 0) == 0 || code.find("NoPermission") != std::string::npos ||
        code.find("Unauthorized") != std::string::npos)
        return "没有费用中心权限（需 AliyunBSSReadOnlyAccess）";
    return message.empty() ? code : code + ": " + message.substr(0, 60);
}

// Parses the common envelope. Success = "Success":true, or Code "200"/"Success" when that field is absent.
cJSON* ParseEnvelope(const std::string& body, std::string* error) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        *error = "响应无法解析";
        return nullptr;
    }
    const cJSON* success = cJSON_GetObjectItemCaseSensitive(root, "Success");
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "Code");
    const cJSON* message = cJSON_GetObjectItemCaseSensitive(root, "Message");
    const std::string code_text = cJSON_IsString(code) ? code->valuestring : "";
    bool ok = cJSON_IsBool(success) ? cJSON_IsTrue(success) : (code_text == "200" || code_text == "Success");
    if (!ok) {
        *error = ErrorText(code_text, cJSON_IsString(message) ? message->valuestring : "");
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}

// Amounts come as numbers or as strings that may carry thousands separators ("1,234.50").
bool ReadAmount(const cJSON* item, double* out) {
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return true;
    }
    if (!cJSON_IsString(item)) return false;
    std::string s;
    for (const char* p = item->valuestring; *p; ++p)
        if (*p != ',') s += *p;
    char* end = nullptr;
    const double v = strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    *out = v;
    return true;
}

std::string Beijing(const DateParts& p, bool with_hour) {
    char buf[16];
    snprintf(buf, sizeof(buf), with_hour ? "%04d%02d%02d%02d" : "%04d%02d%02d", p.year, p.month, p.day, p.hour);
    return buf;
}

std::string Iso8601Utc(time_t t) {
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

}  // namespace

// ---- pure helpers -----------------------------------------------------------------------------

std::string Sha1(const std::string& data) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::string m = data;
    m.push_back(static_cast<char>(0x80));
    while (m.size() % 64 != 56) m.push_back('\0');
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(m.data()) + off + i * 4;
            w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out;
    for (uint32_t v : h)
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
    return out;
}

std::string HmacSha1(const std::string& key_in, const std::string& message) {
    std::string key = key_in.size() > 64 ? Sha1(key_in) : key_in;
    key.resize(64, '\0');
    std::string inner(64, '\0'), outer(64, '\0');
    for (int i = 0; i < 64; ++i) {
        inner[i] = static_cast<char>(key[i] ^ 0x36);
        outer[i] = static_cast<char>(key[i] ^ 0x5c);
    }
    return Sha1(outer + Sha1(inner + message));
}

std::string PercentEncode(const std::string& s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

std::string SignedUrl(const std::string& host, const std::map<std::string, std::string>& params,
                      const std::string& secret) {
    std::string query;  // std::map iterates in byte order of the raw keys, which is what the signature wants
    for (const auto& [k, v] : params) query += (query.empty() ? "" : "&") + PercentEncode(k) + "=" + PercentEncode(v);
    const std::string to_sign = "GET&%2F&" + PercentEncode(query);
    const std::string signature = ai::Base64Encode(HmacSha1(secret + "&", to_sign));
    return "https://" + host + "/?" + query + "&Signature=" + PercentEncode(signature);
}

std::string ModelFromInstanceId(const std::string& instance_id) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t semi = instance_id.find(';', start);
        parts.push_back(instance_id.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    if (parts.size() > 2 && !parts[2].empty()) return parts[2];
    if (parts.size() > 3 && !parts[3].empty()) return parts[3];
    return "其他";
}

DateParts BeijingParts(time_t t) {
    t += 8 * 3600;
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    return {tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour};
}

bool ParseBalance(const std::string& body, double* amount, std::string* error) {
    cJSON* root = ParseEnvelope(body, error);
    if (!root) return false;
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "Data");
    const bool ok = ReadAmount(cJSON_GetObjectItemCaseSensitive(data, "AvailableAmount"), amount);
    if (!ok) *error = "响应里没有余额";
    cJSON_Delete(root);
    return ok;
}

bool ParseBill(const std::string& body, DayBill* out, std::string* error) {
    cJSON* root = ParseEnvelope(body, error);
    if (!root) return false;
    const cJSON* items = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "Data"), "Items");
    if (items && !cJSON_IsArray(items)) items = cJSON_GetObjectItemCaseSensitive(items, "Item");  // raw API shape
    std::map<std::string, double> by_model;
    double total = 0;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, items) {
        const cJSON* code = cJSON_GetObjectItemCaseSensitive(item, "ProductCode");
        if (cJSON_IsString(code) && std::string(code->valuestring) != "sfm") continue;  // Bailian only
        double cost = 0;
        if (!ReadAmount(cJSON_GetObjectItemCaseSensitive(item, "PretaxAmount"), &cost)) continue;
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "InstanceID");
        by_model[ModelFromInstanceId(cJSON_IsString(id) ? id->valuestring : "")] += cost;
        total += cost;
    }
    cJSON_Delete(root);
    out->models.clear();
    for (const auto& [model, cost] : by_model) out->models.push_back({model, cost});
    std::sort(out->models.begin(), out->models.end(),
              [](const ModelCost& a, const ModelCost& b) { return a.cost > b.cost; });
    out->total = total;
    out->ok = true;
    return true;
}

const std::vector<ProviderInfo>& Providers() {
    static const std::vector<ProviderInfo> kProviders = {{"bailian", "阿里百炼"}, {"deepseek", "DeepSeek"}};
    return kProviders;
}

void Baselines::Update(double balance, time_t now) {
    const DateParts p = BeijingParts(now);
    const long dk = atol(Beijing(p, false).c_str());
    const long hk = atol(Beijing(p, true).c_str());
    // A new period starts at the first balance seen in it; a balance above the baseline means a top-up.
    if (dk != day_key || balance > day_base) { day_key = dk; day_base = balance; }
    if (hk != hour_key || balance > hour_base) { hour_key = hk; hour_base = balance; }
}

std::string Baselines::Serialize() const {
    char buf[96];
    snprintf(buf, sizeof(buf), "%ld,%.4f,%ld,%.4f", day_key, day_base, hour_key, hour_base);
    return buf;
}

bool Baselines::Parse(const std::string& text) {
    long dk = 0, hk = 0;
    double db = 0, hb = 0;
    if (sscanf(text.c_str(), "%ld,%lf,%ld,%lf", &dk, &db, &hk, &hb) != 4) return false;
    day_key = dk; day_base = db; hour_key = hk; hour_base = hb;
    return true;
}

bool ParseDeepSeekBalance(const std::string& body, DeepSeekBalance* out, std::string* error) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        *error = "响应无法解析";
        return false;
    }
    const cJSON* infos = cJSON_GetObjectItemCaseSensitive(root, "balance_infos");
    if (!cJSON_IsArray(infos)) {
        const cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON* message = cJSON_GetObjectItemCaseSensitive(err, "message");
        *error = cJSON_IsString(message) ? std::string(message->valuestring).substr(0, 60) : "响应里没有余额";
        cJSON_Delete(root);
        return false;
    }
    const cJSON* available = cJSON_GetObjectItemCaseSensitive(root, "is_available");
    out->available = cJSON_IsBool(available) ? cJSON_IsTrue(available) : true;
    out->lines.clear();
    const cJSON* info = nullptr;
    cJSON_ArrayForEach(info, infos) {
        BalanceLine line;
        const cJSON* currency = cJSON_GetObjectItemCaseSensitive(info, "currency");
        line.currency = cJSON_IsString(currency) ? currency->valuestring : "";
        if (!ReadAmount(cJSON_GetObjectItemCaseSensitive(info, "total_balance"), &line.total)) continue;
        ReadAmount(cJSON_GetObjectItemCaseSensitive(info, "granted_balance"), &line.granted);
        ReadAmount(cJSON_GetObjectItemCaseSensitive(info, "topped_up_balance"), &line.topped_up);
        out->lines.push_back(line);
    }
    cJSON_Delete(root);
    if (out->lines.empty()) {
        *error = "响应里没有余额";
        return false;
    }
    out->primary = out->lines[0];
    for (const auto& line : out->lines)
        if (line.currency == "CNY") out->primary = line;
    return true;
}

bool IsValidInterval(int minutes) { return minutes == 10 || minutes == 30 || minutes == 60; }

std::string FormatYuan(double amount) {
    char buf[32];
    snprintf(buf, sizeof(buf), amount >= 1 || amount <= -1 ? "%.2f" : "%.3f", amount);
    return buf;
}

// ---- service ----------------------------------------------------------------------------------

BoardService& BoardService::Get() {
    static BoardService instance;
    return instance;
}

void BoardService::Start(std::unique_ptr<Platform> platform) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (platform_) return;
        platform_ = std::move(platform);
        key_id_ = platform_->Load("ak_id");
        key_secret_ = platform_->Load("ak_secret");
        baselines_.Parse(platform_->Load("base"));
        const int interval = atoi(platform_->Load("interval").c_str());
        if (IsValidInterval(interval)) interval_min_ = interval;
        snap_.started = true;
        snap_.configured = !key_id_.empty() && !key_secret_.empty();
    }
    Notify();
}

bool BoardService::Started() {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_.started;
}

void BoardService::SetOnChange(std::function<void()> on_change) {
    std::lock_guard<std::mutex> lock(mu_);
    on_change_ = std::move(on_change);
}

bool BoardService::HasCredentials() {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_.configured;
}

bool BoardService::SetCredentials(const std::string& id_in, const std::string& secret_in) {
    auto trim = [](const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    };
    const std::string id = trim(id_in), secret = trim(secret_in);
    auto bad = [](const std::string& s) {
        return s.empty() || s.size() > 128 || std::any_of(s.begin(), s.end(), [](unsigned char c) { return isspace(c); });
    };
    if (bad(id) || bad(secret)) return false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!platform_) return false;
        key_id_ = id;
        key_secret_ = secret;
        platform_->Save("ak_id", id);
        platform_->Save("ak_secret", secret);
        snap_.configured = true;
        snap_.error.clear();
    }
    Notify();
    return true;
}

int BoardService::IntervalMinutes() {
    std::lock_guard<std::mutex> lock(mu_);
    return interval_min_;
}

bool BoardService::SetIntervalMinutes(int minutes) {
    if (!IsValidInterval(minutes)) return false;
    std::lock_guard<std::mutex> lock(mu_);
    if (!platform_) return false;
    interval_min_ = minutes;
    platform_->Save("interval", std::to_string(minutes));
    return true;
}

Snapshot BoardService::GetSnapshot() {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_;
}

void BoardService::Notify() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = on_change_;
    }
    if (cb) cb();
}

void BoardService::Refresh(bool force) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!platform_ || !snap_.configured || snap_.loading) return;
        const time_t now = platform_->Now();
        if (!force && snap_.error.empty() && snap_.updated != 0 && now - snap_.updated < interval_min_ * 60) return;
        snap_.loading = true;
        snap_.error.clear();
    }
    Request(Stage::Balance);
}

void BoardService::Request(Stage stage) {
    std::map<std::string, std::string> params;
    std::string secret;
    time_t now;
    {
        std::lock_guard<std::mutex> lock(mu_);
        now = platform_->Now();
        secret = key_secret_;
        params["Version"] = kVersion;
        params["Format"] = "JSON";
        params["AccessKeyId"] = key_id_;
        params["SignatureMethod"] = "HMAC-SHA1";
        params["SignatureVersion"] = "1.0";
        params["Timestamp"] = Iso8601Utc(now);
        char nonce[40];
        snprintf(nonce, sizeof(nonce), "%lld-%u", static_cast<long long>(now), ++counter_);
        params["SignatureNonce"] = nonce;
    }
    if (now < kMinValidTime) {
        Fail("设备时钟未同步，请先联网");
        return;
    }
    if (stage == Stage::Balance) {
        params["Action"] = "QueryAccountBalance";
    } else {
        const DateParts day = BeijingParts(stage == Stage::Yesterday ? now - 24 * 3600 : now);
        char date[16], cycle[8];
        snprintf(date, sizeof(date), "%04d-%02d-%02d", day.year, day.month, day.day);
        snprintf(cycle, sizeof(cycle), "%04d-%02d", day.year, day.month);
        params["Action"] = "DescribeInstanceBill";
        params["BillingCycle"] = cycle;
        params["Granularity"] = "DAILY";
        params["BillingDate"] = date;
        params["MaxResults"] = std::to_string(kBillMaxResults);
    }
    ai::ChatPlatform::HttpRequest request;
    request.method = "GET";
    request.url = SignedUrl(kHost, params, secret);
    request.max_response = kBillMaxResponse;
    request.fallback_ca_pem = kBillRootCaPem;
    platform_->Send(std::move(request), [this, stage](int status, std::string body, std::string transport_error) {
        OnResponse(stage, status, std::move(body), std::move(transport_error));
    });
}

void BoardService::OnResponse(Stage stage, int status, std::string body, std::string transport_error) {
    if (!transport_error.empty()) {
        Fail("网络错误：" + transport_error);
        return;
    }
    std::string error;
    if (stage == Stage::Balance) {
        double balance = 0;
        if (!ParseBalance(body, &balance, &error)) {
            Fail(status >= 400 && error == "响应无法解析" ? "HTTP " + std::to_string(status) : error);
            return;
        }
        ESP_LOGI("TokenBoard", "balance response: status=%d body=%u B parsed=%f", status, static_cast<unsigned>(body.size()),
                 balance);
        {
            std::lock_guard<std::mutex> lock(mu_);
            UpdateBaselinesLocked(balance, platform_->Now());
        }
        Notify();
        Request(Stage::Yesterday);
        return;
    }
    DayBill bill;
    if (!ParseBill(body, &bill, &error)) {
        Fail(status >= 400 && error == "响应无法解析" ? "HTTP " + std::to_string(status) : error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stage == Stage::Yesterday) snap_.yesterday = bill;
        else snap_.today = bill;
        if (stage == Stage::Today) {
            snap_.today_cost = std::max(snap_.today_cost, bill.total);
        }
    }
    if (stage == Stage::Yesterday) {
        Notify();
        Request(Stage::Today);
    } else {
        Finish();
    }
}

void BoardService::UpdateBaselinesLocked(double balance, time_t now) {
    baselines_.Update(balance, now);
    platform_->Save("base", baselines_.Serialize());
    snap_.has_balance = true;
    snap_.balance = balance;
    snap_.today_cost = baselines_.TodayCost(balance);
    snap_.hour_cost = baselines_.HourCost(balance);
    snap_.updated = now;
}

void BoardService::Fail(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_.loading = false;
        snap_.error = error;
    }
    Notify();
}

void BoardService::Finish() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_.loading = false;
    }
    Notify();
}

// ---- DeepSeek ---------------------------------------------------------------------------------

DeepSeekService& DeepSeekService::Get() {
    static DeepSeekService instance;
    return instance;
}

void DeepSeekService::Start(std::unique_ptr<Platform> platform) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (platform_) return;
        platform_ = std::move(platform);
        key_ = platform_->Load("ds_key");
        baselines_.Parse(platform_->Load("ds_base"));
        snap_.started = true;
        snap_.configured = !key_.empty();
    }
    Notify();
}

bool DeepSeekService::Started() {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_.started;
}

void DeepSeekService::SetOnChange(std::function<void()> on_change) {
    std::lock_guard<std::mutex> lock(mu_);
    on_change_ = std::move(on_change);
}

bool DeepSeekService::HasApiKey() {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_.configured;
}

bool DeepSeekService::SetApiKey(const std::string& key_in) {
    size_t a = 0, b = key_in.size();
    while (a < b && isspace(static_cast<unsigned char>(key_in[a]))) ++a;
    while (b > a && isspace(static_cast<unsigned char>(key_in[b - 1]))) --b;
    const std::string key = key_in.substr(a, b - a);
    if (key.empty() || key.size() > 200 ||
        std::any_of(key.begin(), key.end(), [](unsigned char c) { return isspace(c); }))
        return false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!platform_) return false;
        key_ = key;
        platform_->Save("ds_key", key);
        snap_.configured = true;
        snap_.error.clear();
    }
    Notify();
    return true;
}

DeepSeekSnapshot DeepSeekService::GetSnapshot() {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_;
}

void DeepSeekService::Notify() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = on_change_;
    }
    if (cb) cb();
}

void DeepSeekService::Refresh(bool force) {
    const int interval = BoardService::Get().IntervalMinutes();
    ai::ChatPlatform::HttpRequest request;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!platform_ || !snap_.configured || snap_.loading) return;
        const time_t now = platform_->Now();
        if (!force && snap_.error.empty() && snap_.updated != 0 && now - snap_.updated < interval * 60) return;
        snap_.loading = true;
        snap_.error.clear();
        request.method = "GET";
        request.url = "https://api.deepseek.com/user/balance";
        request.headers = {{"Authorization", "Bearer " + key_}, {"Accept", "application/json"}};
    }
    platform_->Send(std::move(request), [this](int status, std::string body, std::string transport_error) {
        OnResponse(status, std::move(body), std::move(transport_error));
    });
}

void DeepSeekService::OnResponse(int status, std::string body, std::string transport_error) {
    if (!transport_error.empty()) {
        Fail("网络错误：" + transport_error);
        return;
    }
    DeepSeekBalance balance;
    std::string error;
    if (!ParseDeepSeekBalance(body, &balance, &error)) {
        // A non-JSON answer with an error status is usually a proxy or web-filter page (403 "Access Denied").
        Fail(status == 401 ? "API Key 无效" : (status >= 400 && error == "响应无法解析" ? "HTTP " + std::to_string(status) : error));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        const time_t now = platform_->Now();
        snap_.has_balance = true;
        snap_.balance = balance;
        if (now >= kMinValidTime) {  // an unsynced clock would put the baselines in 1970
            baselines_.Update(balance.primary.total, now);
            platform_->Save("ds_base", baselines_.Serialize());
            snap_.today_cost = baselines_.TodayCost(balance.primary.total);
            snap_.hour_cost = baselines_.HourCost(balance.primary.total);
            snap_.updated = now;
        }
        snap_.loading = false;
    }
    Notify();
}

void DeepSeekService::Fail(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_.loading = false;
        snap_.error = error;
    }
    Notify();
}

}  // namespace board
