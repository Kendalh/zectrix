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

// AI Q&A (common/ai_chat.h): only the API key is set here; questions are spoken on the device.
esp_err_t AiPageHandler(httpd_req_t* req);          // GET /ai: key setup
esp_err_t AiStateHandler(httpd_req_t* req);         // GET /api/ai/state: whether the key is set
esp_err_t AiKeyHandler(httpd_req_t* req);           // POST /api/ai/key {key}: stored on the device, never returned

}  // namespace web
}  // namespace rawdraw

#endif  // RAWDRAW_WEB_APPS_H
