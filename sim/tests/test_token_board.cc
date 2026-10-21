// Unit tests for common/token_board.cc: SHA1/HMAC/signature (against Python's hashlib/hmac), response parsing,
// and the service's balance baselines (with a scripted platform instead of the network).
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "common/token_board.h"

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)

namespace {

std::string Hex(const std::string& raw) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 15]);
    }
    return out;
}

// Answers every request synchronously from `balance` and the canned bills; keeps NVS-like storage and a fake clock.
class ScriptedPlatform : public board::Platform {
public:
    void Send(ai::ChatPlatform::HttpRequest r, ai::ChatPlatform::DoneCallback done) override {
        urls.push_back(r.url);
        for (const auto& h : r.headers)
            if (h.first == "Authorization") last_auth = h.second;
        if (r.url.find("api.deepseek.com") != std::string::npos) {
            done(ds_status, ds_body, "");
        } else if (r.url.find("Action=QueryAccountBalance") != std::string::npos) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "{\"Code\":\"200\",\"Success\":true,\"Data\":{\"AvailableAmount\":\"%s\"}}", balance.c_str());
            done(200, buf, "");
        } else if (fail_bills) {
            done(400, "{\"Code\":\"SignatureDoesNotMatch\",\"Message\":\"bad\"}", "");
        } else {
            done(200, bill, "");
        }
    }
    std::string Load(const std::string& key) override { return store.count(key) ? store[key] : ""; }
    void Save(const std::string& key, const std::string& value) override { store[key] = value; }
    time_t Now() override { return now; }

    std::string balance = "100.00";
    int ds_status = 200;
    std::string ds_body =
        "{\"is_available\":true,\"balance_infos\":[{\"currency\":\"CNY\",\"total_balance\":\"110.00\","
        "\"granted_balance\":\"10.00\",\"topped_up_balance\":\"100.00\"}]}";
    std::string last_auth;
    std::string bill = "{\"Code\":\"Success\",\"Success\":true,\"Data\":{\"Items\":{\"Item\":[]}}}";
    bool fail_bills = false;
    time_t now = 1792467600;  // 2026-10-20 03:40:00 UTC = 11:40 Beijing
    std::map<std::string, std::string> store;
    std::vector<std::string> urls;
};

// The service owns its platform; the test keeps a raw pointer to script it.
board::BoardService* NewService(ScriptedPlatform** out, std::map<std::string, std::string> store = {}) {
    auto* svc = new board::BoardService();
    auto platform = std::make_unique<ScriptedPlatform>();
    platform->store = std::move(store);
    *out = platform.get();
    svc->Start(std::move(platform));
    return svc;
}

void TestSignature() {
    CHECK(Hex(board::Sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(Hex(board::Sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(Hex(board::HmacSha1("key", "The quick brown fox jumps over the lazy dog")) ==
          "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9");
    // A key longer than the 64-byte block is hashed first (checked against hmac.new(...) in Python).
    CHECK(Hex(board::HmacSha1(std::string(100, 'k'), "m")) == "014f08e9aabf7980d07a205a4c638743915ff915");
    CHECK(board::PercentEncode("a b~c/d:e") == "a%20b~c%2Fd%3Ae");

    const std::map<std::string, std::string> params = {
        {"Action", "QueryAccountBalance"}, {"Version", "2017-12-14"}, {"Format", "JSON"},
        {"AccessKeyId", "testid"}, {"SignatureMethod", "HMAC-SHA1"}, {"SignatureVersion", "1.0"},
        {"SignatureNonce", "n-1"}, {"Timestamp", "2026-10-20T03:26:05Z"}};
    CHECK(board::SignedUrl("business.aliyuncs.com", params, "testsecret") ==
          "https://business.aliyuncs.com/?AccessKeyId=testid&Action=QueryAccountBalance&Format=JSON&"
          "SignatureMethod=HMAC-SHA1&SignatureNonce=n-1&SignatureVersion=1.0&Timestamp=2026-10-20T03%3A26%3A05Z&"
          "Version=2017-12-14&Signature=Xw%2FeZizxmOyX42BOHzn%2B13CCPRU%3D");
}

void TestParsing() {
    CHECK(board::ModelFromInstanceId("7839675;llm-i1auu0dmo6t80ckx;qwen-flash;context_0-128k_input_token;;0") == "qwen-flash");
    CHECK(board::ModelFromInstanceId("cn-beijing;sfm_plugin_public_cn-zqb4obkdqfk;;web-search-turbo") == "web-search-turbo");
    CHECK(board::ModelFromInstanceId("") == "其他");

    const board::DateParts p = board::BeijingParts(1792467600);
    CHECK(p.year == 2026 && p.month == 10 && p.day == 20 && p.hour == 11);
    const board::DateParts rollover = board::BeijingParts(1792467600 + 13 * 3600);  // 16:00 UTC = 00:40 next day
    CHECK(rollover.day == 21 && rollover.hour == 0);

    CHECK(board::FormatYuan(3.72) == "3.72");
    CHECK(board::FormatYuan(0.0053454) == "0.005");
    CHECK(board::FormatYuan(16) == "16.00");

    double amount = 0;
    std::string error;
    CHECK(board::ParseBalance("{\"Code\":\"200\",\"Success\":true,\"Data\":{\"AvailableAmount\":\"1,234.50\"}}", &amount, &error));
    CHECK(amount == 1234.5);
    CHECK(!board::ParseBalance("{\"Code\":\"InvalidAccessKeyId.NotFound\",\"Message\":\"x\",\"Success\":false}", &amount, &error));
    CHECK(error == "AccessKey ID 无效");
    CHECK(!board::ParseBalance("not json", &amount, &error));
    CHECK(!board::ParseBalance("{\"Code\":\"200\",\"Data\":{}}", &amount, &error));

    // Raw API shape (Items.Item) and the SDK's flattened shape (Items array); only ProductCode "sfm" counts, per model.
    const std::string rows =
        "{\"ProductCode\":\"sfm\",\"InstanceID\":\"1;k;qwen-flash;input;;0\",\"PretaxAmount\":0.5},"
        "{\"ProductCode\":\"sfm\",\"InstanceID\":\"1;k;qwen-flash;output;;0\",\"PretaxAmount\":0.25},"
        "{\"ProductCode\":\"sfm\",\"InstanceID\":\"1;k;qwen-image;images;;0\",\"PretaxAmount\":16},"
        "{\"ProductCode\":\"ecs\",\"InstanceID\":\"i-1\",\"PretaxAmount\":99}";
    for (const std::string& body :
         {"{\"Code\":\"Success\",\"Success\":true,\"Data\":{\"Items\":{\"Item\":[" + rows + "]}}}",
          "{\"Code\":\"Success\",\"Success\":true,\"Data\":{\"Items\":[" + rows + "]}}"}) {
        board::DayBill bill;
        CHECK(board::ParseBill(body, &bill, &error));
        CHECK(bill.ok && bill.models.size() == 2);
        CHECK(bill.models[0].model == "qwen-image" && bill.models[0].cost == 16);
        CHECK(bill.models[1].model == "qwen-flash" && bill.models[1].cost == 0.75);
        CHECK(bill.total == 16.75);
    }
    board::DayBill empty;
    CHECK(board::ParseBill("{\"Code\":\"Success\",\"Success\":true,\"Data\":{\"Items\":{\"Item\":[]}}}", &empty, &error));
    CHECK(empty.ok && empty.models.empty() && empty.total == 0);
}

void TestService() {
    ScriptedPlatform* p = nullptr;
    std::unique_ptr<board::BoardService> svc(NewService(&p));
    CHECK(!svc->HasCredentials());
    svc->Refresh(true);  // no key: nothing happens
    CHECK(p->urls.empty());
    CHECK(!svc->SetCredentials("", "secret"));
    CHECK(!svc->SetCredentials("id with space", "secret"));
    CHECK(svc->SetCredentials(" LTAIxyz ", "secret "));
    CHECK(svc->HasCredentials() && p->store["ak_id"] == "LTAIxyz" && p->store["ak_secret"] == "secret");

    // First refresh: three signed requests (balance, yesterday, today); baselines start at the balance seen.
    svc->Refresh(true);
    CHECK(p->urls.size() == 3);
    CHECK(p->urls[0].find("Action=QueryAccountBalance") != std::string::npos);
    CHECK(p->urls[1].find("BillingDate=2026-10-19") != std::string::npos);
    CHECK(p->urls[2].find("BillingDate=2026-10-20") != std::string::npos);
    CHECK(p->urls[1].find("Granularity=DAILY") != std::string::npos);
    CHECK(p->urls[1].find("AccessKeyId=LTAIxyz") != std::string::npos && p->urls[1].find("secret") == std::string::npos);
    board::Snapshot s = svc->GetSnapshot();
    CHECK(s.has_balance && s.balance == 100 && !s.loading && s.error.empty());
    CHECK(s.today_cost == 0 && s.hour_cost == 0);
    CHECK(s.yesterday.ok && s.today.ok);

    // Not stale: a non-forced refresh does nothing; a forced one runs.
    svc->Refresh(false);
    CHECK(p->urls.size() == 3);
    p->now += 30 * 60;  // 12:10 Beijing: a new hour, same day
    p->balance = "99.40";
    svc->Refresh(true);
    s = svc->GetSnapshot();
    CHECK(s.balance == 99.4);
    CHECK(s.today_cost > 0.59 && s.today_cost < 0.61);  // 100 - 99.40
    CHECK(s.hour_cost == 0);                             // the hour baseline restarted at 99.40
    p->now += 10 * 60;
    p->balance = "99.10";
    svc->Refresh(true);
    s = svc->GetSnapshot();
    CHECK(s.hour_cost > 0.29 && s.hour_cost < 0.31);
    CHECK(s.today_cost > 0.89 && s.today_cost < 0.91);

    // Refresh interval: default 1 h, only 10/30/60 accepted, persisted, and it sets when data counts as stale.
    CHECK(svc->IntervalMinutes() == 60);
    CHECK(!svc->SetIntervalMinutes(15) && !svc->SetIntervalMinutes(0) && svc->IntervalMinutes() == 60);
    const size_t calls = p->urls.size();
    p->now += 20 * 60;
    svc->Refresh(false);
    CHECK(p->urls.size() == calls);          // 20 min old, interval 1 h: still fresh
    CHECK(svc->SetIntervalMinutes(10) && p->store["interval"] == "10");
    svc->Refresh(false);
    CHECK(p->urls.size() == calls + 3);      // 20 min old, interval 10 min: refetched
    CHECK(board::Providers().size() >= 1 && std::string(board::Providers()[0].name) == "阿里百炼");

    // Today's bill total wins when it is larger than the balance estimate.
    p->bill = "{\"Code\":\"Success\",\"Success\":true,\"Data\":{\"Items\":[{\"ProductCode\":\"sfm\",\"InstanceID\":\"1;k;m;t;;0\",\"PretaxAmount\":5}]}}";
    svc->Refresh(true);
    s = svc->GetSnapshot();
    CHECK(s.today_cost == 5);

    // A top-up (balance above the baseline) restarts the baselines instead of going negative.
    p->balance = "200.00";
    svc->Refresh(true);
    s = svc->GetSnapshot();
    CHECK(s.hour_cost == 0 && s.today_cost >= 0);

    // Baselines survive a restart in the same day/hour.
    ScriptedPlatform* p2 = nullptr;
    std::unique_ptr<board::BoardService> svc2(NewService(&p2, p->store));
    CHECK(svc2->HasCredentials() && svc2->IntervalMinutes() == 10);
    p2->now = p->now;
    p2->balance = "199.00";
    svc2->Refresh(true);
    s = svc2->GetSnapshot();
    CHECK(s.hour_cost > 0.99 && s.hour_cost < 1.01);

    // Errors: a rejected bill call keeps the balance and reports the reason; the next non-forced refresh retries.
    p2->fail_bills = true;
    svc2->Refresh(true);
    s = svc2->GetSnapshot();
    CHECK(s.error == "AccessKey Secret 不对" && !s.loading && s.has_balance);
    p2->fail_bills = false;
    const size_t before = p2->urls.size();
    svc2->Refresh(false);
    CHECK(p2->urls.size() == before + 3);

    // An unsynced clock is reported instead of sending a request that would be rejected.
    ScriptedPlatform* p3 = nullptr;
    std::unique_ptr<board::BoardService> svc3(NewService(&p3));
    svc3->SetCredentials("id", "secret");
    p3->now = 1000;
    svc3->Refresh(true);
    CHECK(p3->urls.empty());
    CHECK(svc3->GetSnapshot().error.find("时钟") != std::string::npos);
}

void TestDeepSeek() {
    board::DeepSeekBalance b;
    std::string error;
    CHECK(board::ParseDeepSeekBalance(
        "{\"is_available\":true,\"balance_infos\":[{\"currency\":\"USD\",\"total_balance\":\"5.00\",\"granted_balance\":\"0\","
        "\"topped_up_balance\":\"5.00\"},{\"currency\":\"CNY\",\"total_balance\":\"110.50\",\"granted_balance\":\"10.00\","
        "\"topped_up_balance\":\"100.50\"}]}", &b, &error));
    CHECK(b.available && b.lines.size() == 2);
    CHECK(b.primary.currency == "CNY" && b.primary.total == 110.5 && b.primary.granted == 10 && b.primary.topped_up == 100.5);
    CHECK(board::ParseDeepSeekBalance("{\"is_available\":false,\"balance_infos\":[{\"currency\":\"USD\",\"total_balance\":\"0.00\"}]}", &b, &error));
    CHECK(!b.available && b.primary.currency == "USD");  // no CNY line: the first one
    CHECK(!board::ParseDeepSeekBalance("{\"error\":{\"message\":\"Authentication Fails\"}}", &b, &error));
    CHECK(error == "Authentication Fails");
    CHECK(!board::ParseDeepSeekBalance("{\"balance_infos\":[]}", &b, &error));
    CHECK(!board::ParseDeepSeekBalance("nope", &b, &error));

    ScriptedPlatform* p = nullptr;
    auto* raw = new board::DeepSeekService();
    std::unique_ptr<board::DeepSeekService> svc(raw);
    auto platform = std::make_unique<ScriptedPlatform>();
    p = platform.get();
    svc->Start(std::move(platform));
    CHECK(!svc->HasApiKey());
    svc->Refresh(true);
    CHECK(p->urls.empty());
    CHECK(!svc->SetApiKey("") && !svc->SetApiKey("sk bad"));
    CHECK(svc->SetApiKey(" sk-abc123 ") && svc->HasApiKey() && p->store["ds_key"] == "sk-abc123");

    svc->Refresh(true);
    CHECK(p->urls.size() == 1 && p->urls[0] == "https://api.deepseek.com/user/balance");
    CHECK(p->last_auth == "Bearer sk-abc123");
    board::DeepSeekSnapshot s = svc->GetSnapshot();
    CHECK(s.has_balance && s.balance.primary.total == 110 && s.today_cost == 0 && s.hour_cost == 0 && !s.loading);
    CHECK(s.updated == p->now);

    p->now += 5 * 60;  // within the default 1 h: not refetched; forced is
    svc->Refresh(false);
    CHECK(p->urls.size() == 1);
    p->ds_body = "{\"is_available\":true,\"balance_infos\":[{\"currency\":\"CNY\",\"total_balance\":\"109.40\",\"granted_balance\":\"10.00\",\"topped_up_balance\":\"99.40\"}]}";
    svc->Refresh(true);
    s = svc->GetSnapshot();
    CHECK(s.today_cost > 0.59 && s.today_cost < 0.61 && s.hour_cost > 0.59 && s.hour_cost < 0.61);
    CHECK(p->store.count("ds_base"));

    p->ds_status = 401;
    p->ds_body = "{\"error\":{\"message\":\"Authentication Fails\"}}";
    svc->Refresh(true);
    s = svc->GetSnapshot();
    CHECK(s.error == "API Key 无效" && !s.loading && s.has_balance);

    // A filter page (HTML with HTTP 403) is reported by its status, not as an unreadable response.
    p->ds_status = 403;
    p->ds_body = "<!DOCTYPE html><html>Access Denied</html>";
    svc->Refresh(true);
    CHECK(svc->GetSnapshot().error == "HTTP 403");

    // Baselines survive a restart.
    p->ds_status = 200;
    p->ds_body = "{\"is_available\":true,\"balance_infos\":[{\"currency\":\"CNY\",\"total_balance\":\"109.00\"}]}";
    board::DeepSeekService restarted;
    auto platform2 = std::make_unique<ScriptedPlatform>();
    ScriptedPlatform* p2 = platform2.get();
    p2->store = p->store;
    p2->now = p->now;
    p2->ds_body = p->ds_body;
    restarted.Start(std::move(platform2));
    CHECK(restarted.HasApiKey());
    restarted.Refresh(true);
    CHECK(restarted.GetSnapshot().hour_cost > 0.99 && restarted.GetSnapshot().hour_cost < 1.01);
}

}  // namespace

int main() {
    TestSignature();
    TestParsing();
    TestService();
    TestDeepSeek();
    if (g_failed) {
        fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("test_token_board: all checks passed\n");
    return 0;
}
