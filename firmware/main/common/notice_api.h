/**
 * @file notice_api.h
 * @brief Latest notice (one line of plain text) shown at the bottom of the calendar app
 *
 * A task GETs kNoticeUrl, but **only when asked**: nothing is fetched at boot, when Wi-Fi connects, or on a timer. The calendar
 * app asks when it is entered (notice_api_refresh_if_stale). The callback gets the first line of the response, or "" when the
 * request failed or fetching is switched off (the calendar then shows no notice). The interval is a persisted setting, edited
 * on the device web page: how long a fetched notice stays valid (entering the app again earlier does not fetch).
 */

#ifndef NOTICE_API_H
#define NOTICE_API_H

#include <functional>
#include <string>

constexpr char kNoticeUrl[] = "https://coffeelab.xin/fin/notice.txt";

/** Called on the notice task. */
using NoticeCallback = std::function<void(const std::string& notice)>;

/** Refresh interval in minutes: 10, 30 or 60, or 0 = never fetch. Default 30. */
int notice_api_get_interval();

/** Validate, persist and apply: 0 clears the notice, any other value makes the next app entry fetch. Never fetches by itself. */
bool notice_api_set_interval(int minutes);

/** Create the (idle) task (idempotent). Call after Wi-Fi is connected; it fetches nothing until asked. */
void notice_api_start(NoticeCallback callback);

/**
 * The calendar app was entered: fetch only if the last successful fetch is older than the interval (a fresh notice is already
 * shown) and the interval is not 0. @return false if the task is not running (Wi-Fi not up yet).
 */
bool notice_api_refresh_if_stale();

#endif  // NOTICE_API_H
