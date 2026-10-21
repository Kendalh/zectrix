/**
 * @file token_board.h
 * @brief Token board: Alibaba Bailian balance and cost (Alibaba Cloud BSS OpenAPI), shown on one page.
 *
 * Numbers shown: account balance (QueryAccountBalance), yesterday's and today's cost by model (DescribeInstanceBill,
 * DAILY), and today's / this hour's cost. The bill API has no usable hourly granularity (HOURLY ignores BillingDate and
 * returns the month so far) and today's daily bill lags, so those two are estimated from the balance: the first balance
 * seen in the day/hour is remembered (NVS) and the cost is baseline - balance. Today's cost is the larger of that and
 * today's bill total. A top-up resets the baseline. The bill carries money only, no token counts.
 *
 * Everything here is hardware-free and shared with the simulator. HTTP and key storage sit behind board::Platform
 * (firmware: common/token_board_platform_esp.cc, sim: fake/token_board_platform_fake.cc). Requests use the RPC
 * signature v1 (HMAC-SHA1, GET with a signed query), implemented here so it can be unit-tested.
 * The AccessKey is entered on the device web page (/tokenboard) and never compiled in or sent back.
 * Nothing is fetched in the background: the page asks when it is entered and on BOOT. The refresh interval setting
 * (10 min / 30 min / 1 h, default 1 h, web page /tokenboard) is how old the data may get before entering the page
 * fetches again, not a polling period (same convention as the weather page).
 *
 * Providers: the page shows one provider at a time and UP/DOWN switch between them (Providers()). BoardService is the
 * Alibaba Bailian one, DeepSeekService the DeepSeek one (balance only: DeepSeek has no usage or bill API an API key can
 * call, so there is no per-model breakdown; today/this hour are estimated from the balance like for Bailian). A new
 * provider adds an entry to Providers(), its own service and a render branch in token_board_renderer.cc.
 */

#ifndef TOKEN_BOARD_H
#define TOKEN_BOARD_H

#include <time.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ai_chat.h"

namespace board {

struct ModelCost {
    std::string model;
    double cost = 0;
};

struct DayBill {
    bool ok = false;                  // the bill was fetched (an empty day is ok with no models)
    std::vector<ModelCost> models;    // Bailian only, highest cost first
    double total = 0;
};

struct Snapshot {
    bool started = false;
    bool configured = false;          // an AccessKey is stored
    bool loading = false;
    std::string error;                // why the last refresh failed (Chinese, user-facing)
    bool has_balance = false;
    double balance = 0;
    double hour_cost = 0;             // estimate from the balance, see the file comment
    double today_cost = 0;
    DayBill yesterday;
    DayBill today;
    time_t updated = 0;               // last refresh that got the balance
};

struct ProviderInfo {
    const char* id;    // stable identifier
    const char* name;  // shown on the page
};

/** The providers the page can show, in page order. */
const std::vector<ProviderInfo>& Providers();

constexpr int kDefaultIntervalMinutes = 60;
/** 10, 30 or 60. */
bool IsValidInterval(int minutes);

/** Balance of one day/hour as the first balance seen in it; a rise means a top-up and restarts the baseline. */
struct Baselines {
    long day_key = 0, hour_key = 0;  // Beijing YYYYMMDD / YYYYMMDDHH
    double day_base = 0, hour_base = 0;

    void Update(double balance, time_t now);
    double TodayCost(double balance) const { return day_base - balance; }
    double HourCost(double balance) const { return hour_base - balance; }
    std::string Serialize() const;
    bool Parse(const std::string& text);
};

struct DateParts {
    int year = 0, month = 0, day = 0, hour = 0;
};

// ---- pure helpers (unit-tested) ---------------------------------------------------------------

std::string Sha1(const std::string& data);                                  // raw 20 bytes
std::string HmacSha1(const std::string& key, const std::string& message);   // raw 20 bytes
std::string PercentEncode(const std::string& s);                            // RFC 3986, as the signature wants it

/** `https://host/?` + the sorted, encoded params + `&Signature=...` (RPC signature v1, key = secret + "&"). */
std::string SignedUrl(const std::string& host, const std::map<std::string, std::string>& params,
                      const std::string& secret);

/** Model of a bill row from its InstanceID ("acct;apikey;model;type;;0"; plugins: "region;id;;tool"). */
std::string ModelFromInstanceId(const std::string& instance_id);

/** Beijing time (UTC+8, the billing time zone) of a Unix time. */
DateParts BeijingParts(time_t t);

bool ParseBalance(const std::string& body, double* amount, std::string* error);
/** Bailian rows (ProductCode "sfm") of a DescribeInstanceBill response, summed per model. */
bool ParseBill(const std::string& body, DayBill* out, std::string* error);

struct BalanceLine {
    std::string currency;  // "CNY" or "USD"
    double total = 0;      // includes granted and topped-up: never add the three
    double granted = 0;
    double topped_up = 0;
};

struct DeepSeekBalance {
    bool available = true;                 // is_available: enough balance to call the API
    std::vector<BalanceLine> lines;        // one per currency
    BalanceLine primary;                   // the CNY line, else the first
};

/** Response of GET https://api.deepseek.com/user/balance. */
bool ParseDeepSeekBalance(const std::string& body, DeepSeekBalance* out, std::string* error);

/** "3.72", or 3 decimals below 1 yuan so that small model costs do not all show 0.00. */
std::string FormatYuan(double amount);

// ---- platform ---------------------------------------------------------------------------------

class Platform {
public:
    virtual ~Platform() = default;
    /** Same contract as ai::ChatPlatform::Send. */
    virtual void Send(ai::ChatPlatform::HttpRequest request, ai::ChatPlatform::DoneCallback done) = 0;
    virtual std::string Load(const std::string& key) = 0;                       // "" when unset
    virtual void Save(const std::string& key, const std::string& value) = 0;
    virtual time_t Now() { return time(nullptr); }
};

// ---- service ----------------------------------------------------------------------------------

class BoardService {
public:
    static BoardService& Get();

    /** Idempotent. Loads the stored AccessKey and the balance baselines. */
    void Start(std::unique_ptr<Platform> platform);
    bool Started();
    /** Called (any thread, no internal lock held) whenever the snapshot changed. */
    void SetOnChange(std::function<void()> on_change);

    bool HasCredentials();
    /** Trimmed; empty or whitespace-containing values are rejected. */
    bool SetCredentials(const std::string& id, const std::string& secret);

    /** Fetch now (force) or only when the data is older than the interval or the last try failed. No-op while loading. */
    void Refresh(bool force);

    int IntervalMinutes();
    bool SetIntervalMinutes(int minutes);  // persisted; false unless IsValidInterval

    Snapshot GetSnapshot();

private:
    enum class Stage { Balance, Yesterday, Today };
    void Request(Stage stage);
    void OnResponse(Stage stage, int status, std::string body, std::string transport_error);
    void Fail(const std::string& error);
    void Finish();
    void UpdateBaselinesLocked(double balance, time_t now);
    void Notify();

    std::mutex mu_;
    std::unique_ptr<Platform> platform_;
    std::function<void()> on_change_;
    std::string key_id_, key_secret_;
    Snapshot snap_;
    Baselines baselines_;
    int interval_min_ = kDefaultIntervalMinutes;
    unsigned counter_ = 0;                     // nonce uniqueness within one second

public:
    BoardService() = default;  // public for tests; the app uses Get()
};

struct DeepSeekSnapshot {
    bool started = false;
    bool configured = false;           // an API key is stored
    bool loading = false;
    std::string error;                 // Chinese, user-facing
    bool has_balance = false;
    DeepSeekBalance balance;
    double hour_cost = 0;              // estimates from the balance
    double today_cost = 0;
    time_t updated = 0;
};

/** DeepSeek: one request, GET /user/balance with the API key as bearer token. Same refresh rules as BoardService. */
class DeepSeekService {
public:
    static DeepSeekService& Get();

    void Start(std::unique_ptr<Platform> platform);  // loads the key ("ds_key") and baselines ("ds_base")
    bool Started();
    void SetOnChange(std::function<void()> on_change);

    bool HasApiKey();
    bool SetApiKey(const std::string& key);  // trimmed; empty or whitespace-containing keys are rejected

    /** The interval is the shared one of BoardService::IntervalMinutes(). */
    void Refresh(bool force);
    DeepSeekSnapshot GetSnapshot();

private:
    void OnResponse(int status, std::string body, std::string transport_error);
    void Fail(const std::string& error);
    void Notify();

    std::mutex mu_;
    std::unique_ptr<Platform> platform_;
    std::function<void()> on_change_;
    std::string key_;
    DeepSeekSnapshot snap_;
    Baselines baselines_;

public:
    DeepSeekService() = default;  // public for tests; the app uses Get()
};

}  // namespace board

#endif  // TOKEN_BOARD_H
