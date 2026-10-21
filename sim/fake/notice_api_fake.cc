// Host stand-in for common/notice_api.cc. No network in the simulator: the notice is whatever the
// SIM_NOTICE environment variable holds (unset = fetch failed = no notice line). Like the device it "fetches" only when the
// calendar app is entered (notice_api_refresh_if_stale), never at start. The interval is kept in memory; 0 (never fetch)
// hides the notice, any other value shows it again the next time the calendar is entered.
#include "common/notice_api.h"

#include <cstdlib>
#include <mutex>

namespace {
std::mutex g_mutex;
NoticeCallback g_callback;
int g_interval = 30;
bool g_started = false;
bool g_fetched = false;  // a notice was "fetched" and is still valid
int g_fetch_count = 0;   // how often the "server" was asked (tests)

void Deliver() {
    const char* text = std::getenv("SIM_NOTICE");
    if (g_callback) g_callback(g_interval != 0 && text ? text : "");
}
}  // namespace

int notice_api_get_interval() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_interval;
}

bool notice_api_set_interval(int minutes) {
    if (minutes != 0 && minutes != 10 && minutes != 30 && minutes != 60) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_interval = minutes;
    g_fetched = false;
    if (minutes == 0 && g_callback) g_callback("");  // cleared at once; any other value waits for the next entry
    return true;
}

void notice_api_start(NoticeCallback callback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_callback = std::move(callback);
    g_started = true;
}

bool notice_api_refresh_if_stale() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_started) return false;
    if (g_interval == 0 || g_fetched) return true;  // never fetching, or the shown notice is still valid
    g_fetched = true;
    ++g_fetch_count;
    Deliver();
    return true;
}

int sim_notice_fetch_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_fetch_count;
}
