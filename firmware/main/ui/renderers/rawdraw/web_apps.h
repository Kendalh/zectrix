/**
 * @file web_apps.h
 * @brief Device web UI: app launcher home page, the weather settings app and the AI Q&A page.
 *
 * Handlers are registered by ApTransferServer. To add another app to the home page, add a card
 * to kHomeHtml in web_apps.cc and register its page/API handlers in ApTransferServer.
 */

#ifndef RAWDRAW_WEB_APPS_H
#define RAWDRAW_WEB_APPS_H

#include <esp_http_server.h>

#include <string>

namespace rawdraw {
namespace web {

/** Percent-decode a query value ('+' becomes space). */
std::string UrlDecode(const char* in);

esp_err_t HomeHandler(httpd_req_t* req);            // GET /
esp_err_t WeatherPageHandler(httpd_req_t* req);     // GET /weather
esp_err_t WeatherConfigHandler(httpd_req_t* req);   // GET/POST /api/weather
esp_err_t WeatherSearchHandler(httpd_req_t* req);   // GET /api/weather/search?q=

// Calendar reminders (common/reminders.h): edited in the browser, shown on the device's calendar app.
esp_err_t CalendarPageHandler(httpd_req_t* req);    // GET /calendar
esp_err_t RemindersHandler(httpd_req_t* req);       // GET /api/reminders; POST {date, items:[...]} replaces one date
esp_err_t NoticeConfigHandler(httpd_req_t* req);    // GET/POST /api/notice {interval: 0|10|30|60 minutes}
esp_err_t FestivalsHandler(httpd_req_t* req);       // GET /api/festivals?year=YYYY: Chinese festivals by date

// AI Q&A (common/ai_chat.h): only the API key is set here; questions are spoken on the device.
esp_err_t AiPageHandler(httpd_req_t* req);          // GET /ai: key setup
esp_err_t AiStateHandler(httpd_req_t* req);         // GET /api/ai/state: whether the key is set
esp_err_t AiKeyHandler(httpd_req_t* req);           // POST /api/ai/key {key}: stored on the device, never returned

// Token board (common/token_board.h): only the Alibaba Cloud AccessKey is set here; the board is read on the device.
esp_err_t TokenBoardPageHandler(httpd_req_t* req);   // GET /tokenboard: AccessKey setup
esp_err_t TokenBoardStateHandler(httpd_req_t* req);  // GET /api/tokenboard/state: whether a key is set
esp_err_t TokenBoardDeepSeekHandler(httpd_req_t* req);  // POST /api/tokenboard/deepseek {key}: DeepSeek API key, stored, never returned
esp_err_t TokenBoardIntervalHandler(httpd_req_t* req);  // POST /api/tokenboard/interval {interval: 10|30|60 minutes}
esp_err_t TokenBoardKeyHandler(httpd_req_t* req);    // POST /api/tokenboard/key {id, secret}: stored on the device, never returned

}  // namespace web
}  // namespace rawdraw

#endif  // RAWDRAW_WEB_APPS_H
