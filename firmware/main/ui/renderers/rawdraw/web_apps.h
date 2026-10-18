/**
 * @file web_apps.h
 * @brief Device web UI: app launcher home page and the weather settings app.
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

}  // namespace web
}  // namespace rawdraw

#endif  // RAWDRAW_WEB_APPS_H
