/**
 * @file web_apps.cc
 * @brief Device web UI: home page (app launcher) and weather settings. See web_apps.h.
 */

#include "web_apps.h"

#include "common/ai_chat.h"
#include "common/weather_api.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace rawdraw {
namespace web {

namespace {

constexpr const char* kTag = "WebApps";

const char kStyle[] = R"CSS(
*{box-sizing:border-box}body{margin:0;background:#ece8dc;color:#171717;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;font-size:13px}
.app{max-width:520px;margin:0 auto;padding:12px}.top{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}
.brand{font-weight:800;font-size:17px}a{color:inherit}.muted{color:#555}
.panel{background:#fff;border:2px solid #111;border-radius:6px;box-shadow:3px 3px 0 #111;margin-bottom:12px;padding:12px}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.btn{border:2px solid #111;background:#ff3b30;color:#fff;border-radius:5px;padding:8px 12px;font-weight:800;font-size:13px;box-shadow:2px 2px 0 #111;cursor:pointer}
.btn.yellow{background:#ffd900;color:#111}.btn.secondary{background:#fff;color:#111}.btn:disabled{opacity:.5}
input[type=text]{flex:1;min-width:0;padding:8px;border:1px solid #111;font-size:14px}
.card{display:block;text-decoration:none;color:inherit}.card .t{font-weight:800;font-size:16px}
.city{display:block;width:100%;text-align:left;background:#fff;border:1px solid #111;padding:8px;margin-top:6px;border-radius:4px;font-size:14px;cursor:pointer}
.city.sel{background:#ffd900}
)CSS";

const char kHomeHtml[] = R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>墨水屏设置</title>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"><style>%STYLE%</style></head><body><main class="app">
<div class="top"><div><div class="brand">墨水屏设置</div><div class="muted" id="ep"></div></div></div>
<a class="panel card" href="/album"><div class="t">相册 · 传图</div><div class="muted">上传照片、填写名字、管理设备相册和轮播周期</div></a>
<a class="panel card" href="/weather"><div class="t">天气</div><div class="muted" id="wsum">管理最多 5 个城市和刷新频率</div></a>
<a class="panel card" href="/ai"><div class="t">AI 问答 · Key 设置</div><div class="muted">填写阿里云百炼的 API Key（提问在设备上用语音完成）</div></a>
</main><script>
fetch('/status').then(r=>r.json()).then(j=>{document.getElementById('ep').textContent=j.ip}).catch(()=>{});
fetch('/api/weather').then(r=>r.json()).then(j=>{document.getElementById('wsum').textContent=j.cities[j.selected].name+' · 共 '+j.cities.length+' 个城市 · '+j.interval+' 分钟刷新'}).catch(()=>{});
</script></body></html>)HTML";

const char kWeatherHtml[] = R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>天气设置</title>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"><style>%STYLE%
.item{display:flex;align-items:center;gap:8px;border:1px solid #111;border-radius:4px;padding:8px;margin-top:6px}.item.cur{background:#ffd900}.item .n{flex:1;font-size:14px}
.item button{border:1px solid #111;background:#fff;border-radius:4px;padding:5px 8px;cursor:pointer}</style></head><body><main class="app">
<div class="top"><div><div class="brand">天气设置</div><div class="muted">数据来源 Open-Meteo（免费，无需 Key）</div></div><a class="btn secondary" href="/" style="text-decoration:none">首页</a></div>
<section class="panel"><b>当前天气</b><div id="cur" class="muted" style="margin-top:6px">读取中...</div></section>
<section class="panel"><b>我的城市 <span class="muted" id="cnt"></span></b><div class="muted" style="margin-top:4px">设备天气页按 UP/DOWN 切换这些城市；点"设为当前"选择开机显示的城市。</div><div id="list"></div></section>
<section class="panel"><b>添加城市</b>
<div class="row" style="margin-top:8px"><input type="text" id="q" placeholder="输入城市名，如 杭州 / Shanghai"><button class="btn yellow" id="search">搜索</button></div>
<div id="results"></div></section>
<section class="panel"><b>刷新频率</b>
<div class="row" style="margin-top:8px"><label><input name="iv" type="radio" value="5">5 分钟</label><label><input name="iv" type="radio" value="10">10 分钟</label><label><input name="iv" type="radio" value="30" checked>30 分钟</label></div></section>
<div class="row"><button class="btn" id="save">保存并刷新</button><span id="msg" class="muted"></span></div>
</main><script>
const $=id=>document.getElementById(id),MAX=5;let cfg={cities:[],selected:0,interval:30};
function msg(t){$('msg').textContent=t}
function render(){$('cnt').textContent=cfg.cities.length+'/'+MAX;$('list').innerHTML='';
 cfg.cities.forEach((c,i)=>{const d=document.createElement('div');d.className='item'+(i===cfg.selected?' cur':'');
  d.innerHTML='<span class="n"></span>';d.firstChild.textContent=c.name+(c.admin?' · '+c.admin:'')+(i===cfg.selected?'（当前）':'');
  if(i!==cfg.selected){const b=document.createElement('button');b.textContent='设为当前';b.onclick=()=>{cfg.selected=i;render()};d.appendChild(b)}
  if(cfg.cities.length>1){const x=document.createElement('button');x.textContent='删除';x.onclick=()=>{cfg.cities.splice(i,1);if(cfg.selected>=i&&cfg.selected>0)cfg.selected--;render()};d.appendChild(x)}
  $('list').appendChild(d)})}
function show(j){cfg={cities:j.cities,selected:j.selected,interval:j.interval};document.querySelectorAll('input[name=iv]').forEach(r=>r.checked=(+r.value===j.interval));render();
 $('cur').textContent=j.last?(j.last.city+' '+j.last.temp+'°C '+j.last.text+'（更新 '+j.last.time+'）'):'尚未获取到天气（需设备已联网）'}
function load(){return fetch('/api/weather').then(r=>r.json()).then(show).catch(()=>{$('cur').textContent='读取失败'})}
function add(c){if(cfg.cities.length>=MAX){msg('最多 '+MAX+' 个城市');return}
 if(cfg.cities.some(x=>x.lat===c.lat&&x.lon===c.lon)){msg('该城市已添加');return}
 cfg.cities.push({name:c.name,admin:c.admin,lat:c.lat,lon:c.lon});msg('已添加，记得点"保存并刷新"');render()}
$('search').onclick=async()=>{const q=$('q').value.trim();if(!q)return;$('results').innerHTML='<div class="muted" style="margin-top:6px">搜索中...</div>';
 try{const j=await (await fetch('/api/weather/search?q='+encodeURIComponent(q))).json();
  if(!j.success){$('results').innerHTML='<div class="muted" style="margin-top:6px">搜索失败（设备需要联网）</div>';return}
  $('results').innerHTML=j.cities.length?'':'<div class="muted" style="margin-top:6px">没有找到</div>';
  for(const c of j.cities){const d=document.createElement('div');d.className='item';d.innerHTML='<span class="n"></span><button>添加</button>';
   d.firstChild.textContent=c.name+(c.admin?' · '+c.admin:'');d.lastChild.onclick=()=>add(c);$('results').appendChild(d)}
 }catch(e){$('results').innerHTML='<div class="muted" style="margin-top:6px">网络错误</div>'}};
$('q').onkeydown=e=>{if(e.key==='Enter')$('search').click()};
$('save').onclick=async()=>{const iv=+document.querySelector('input[name=iv]:checked').value;$('save').disabled=true;msg('保存中...');
 try{const r=await fetch('/api/weather',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cities:cfg.cities,selected:cfg.selected,interval:iv})});const j=await r.json();
  msg(j.success?'已保存，设备正在刷新天气':'保存失败: '+(j.error||'未知错误'));if(j.success){show(j);setTimeout(load,6000)}}catch(e){msg('网络错误')}$('save').disabled=false};
load();
</script></body></html>)HTML";

const char kAiHtml[] = R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>AI 问答设置</title>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"><style>%STYLE%</style></head><body><main class="app">
<div class="top"><div><div class="brand">AI 问答设置</div><div class="muted">提问和管理会话都在设备上：进入"AI 问答"，长按 UP 说话</div></div><a class="btn secondary" href="/" style="text-decoration:none">首页</a></div>
<section class="panel"><b>阿里云百炼 API Key</b> <span class="muted" id="keyst"></span><div class="muted" style="margin:4px 0 8px">同一个 Key 同时用于语音识别（Qwen3-ASR）和回答（Qwen-Flash）。保存在设备上，不会再显示出来。</div>
<div class="row"><input type="text" id="key" placeholder="sk-..." autocomplete="off"><button class="btn yellow" id="savekey">保存</button></div></section>
<div id="msg" class="muted"></div>
</main><script>
const $=id=>document.getElementById(id);
async function api(path,body){const r=await fetch(path,body?{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}:undefined);return r.json()}
async function load(){try{const j=await api('/api/ai/state');$('keyst').textContent=j.hasKey?'（已设置）':'（未设置）'}catch(e){$('msg').textContent='读取失败（设备离线？）'}}
$('savekey').onclick=async()=>{const j=await api('/api/ai/key',{key:$('key').value});if(j.success){$('key').value='';$('msg').textContent='Key 已保存'}else $('msg').textContent=j.error||'Key 无效';load()};
load();
</script></body></html>)HTML";

void CloseSession(httpd_req_t* req) {
    if (!req || !req->handle) return;
    const int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) httpd_sess_trigger_close(req->handle, sockfd);
}

// Pages embed %STYLE% so the stylesheet exists once in flash.
esp_err_t SendPage(httpd_req_t* req, const char* html) {
    std::string page(html);
    const size_t pos = page.find("%STYLE%");
    if (pos != std::string::npos) page.replace(pos, 7, kStyle);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    const esp_err_t ret = httpd_resp_send(req, page.c_str(), static_cast<ssize_t>(page.size()));
    CloseSession(req);
    return ret;
}

esp_err_t SendJsonTree(httpd_req_t* req, cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    const esp_err_t ret = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    CloseSession(req);
    return ret;
}

esp_err_t SendError(httpd_req_t* req, const char* code) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "error", code);
    return SendJsonTree(req, root);
}

cJSON* ReadBody(httpd_req_t* req, size_t max_len = 2048) {
    if (req->content_len == 0 || req->content_len > max_len) return nullptr;
    std::string buf(req->content_len, '\0');
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, &buf[got], req->content_len - got);
        if (n <= 0) return nullptr;
        got += static_cast<size_t>(n);
    }
    return cJSON_Parse(buf.c_str());
}

}  // namespace

std::string UrlDecode(const char* in) {
    std::string out;
    for (size_t i = 0; in[i]; ++i) {
        if (in[i] == '%' && isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            const char hex[3] = {in[i + 1], in[i + 2], 0};
            out += static_cast<char>(strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out += in[i] == '+' ? ' ' : in[i];
        }
    }
    return out;
}

esp_err_t HomeHandler(httpd_req_t* req) { return SendPage(req, kHomeHtml); }

esp_err_t WeatherPageHandler(httpd_req_t* req) { return SendPage(req, kWeatherHtml); }

esp_err_t WeatherConfigHandler(httpd_req_t* req) {
    if (req->method == HTTP_POST) {
        cJSON* body = ReadBody(req);
        if (!body) return SendError(req, "bad_json");
        WeatherConfig config = weather_api_get_config();
        cJSON* cities = cJSON_GetObjectItemCaseSensitive(body, "cities");
        if (cJSON_IsArray(cities)) {
            config.cities.clear();
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, cities) {
                auto text = [item](const char* key) -> std::string {
                    cJSON* v = cJSON_GetObjectItemCaseSensitive(item, key);
                    return cJSON_IsString(v) ? v->valuestring : "";
                };
                config.cities.push_back({text("name"), text("admin"), text("lat"), text("lon")});
                if (config.cities.size() > kMaxWeatherCities) break;  // rejected by validation below
            }
        }
        cJSON* selected = cJSON_GetObjectItemCaseSensitive(body, "selected");
        cJSON* interval = cJSON_GetObjectItemCaseSensitive(body, "interval");
        if (cJSON_IsNumber(selected)) config.selected = selected->valueint;
        if (cJSON_IsNumber(interval)) config.interval_min = interval->valueint;
        cJSON_Delete(body);
        if (!weather_api_set_config(config)) {
            ESP_LOGW(kTag, "Rejected weather config");
            return SendError(req, "invalid_config");
        }
    }

    const WeatherConfig config = weather_api_get_config();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON* list = cJSON_AddArrayToObject(root, "cities");
    for (const auto& c : config.cities) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", c.name.c_str());
        cJSON_AddStringToObject(item, "admin", c.admin.c_str());
        cJSON_AddStringToObject(item, "lat", c.lat.c_str());
        cJSON_AddStringToObject(item, "lon", c.lon.c_str());
        cJSON_AddItemToArray(list, item);
    }
    cJSON_AddNumberToObject(root, "selected", config.selected);
    cJSON_AddNumberToObject(root, "interval", config.interval_min);
    WeatherData last;
    if (weather_api_get_last_data(&last)) {
        cJSON* l = cJSON_AddObjectToObject(root, "last");
        cJSON_AddStringToObject(l, "city", last.city.c_str());
        cJSON_AddStringToObject(l, "temp", last.temp.c_str());
        cJSON_AddStringToObject(l, "text", last.weather_text.c_str());
        cJSON_AddStringToObject(l, "time", last.update_time.c_str());
    } else {
        cJSON_AddNullToObject(root, "last");
    }
    return SendJsonTree(req, root);
}

esp_err_t WeatherSearchHandler(httpd_req_t* req) {
    char query[160] = {};
    char encoded[128] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "q", encoded, sizeof(encoded)) != ESP_OK || !encoded[0]) {
        return SendError(req, "missing_query");
    }
    // The value is forwarded into a URL as-is, so accept only percent-encoded/URL-safe characters.
    for (const char* p = encoded; *p; ++p) {
        if (!isalnum(static_cast<unsigned char>(*p)) && !strchr("%+-._~", *p)) return SendError(req, "bad_query");
    }
    std::vector<WeatherCity> cities;
    if (!weather_api_search_city(encoded, &cities)) return SendError(req, "search_failed");

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON* list = cJSON_AddArrayToObject(root, "cities");
    for (const auto& c : cities) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", c.name.c_str());
        cJSON_AddStringToObject(item, "admin", c.admin.c_str());
        cJSON_AddStringToObject(item, "lat", c.lat.c_str());
        cJSON_AddStringToObject(item, "lon", c.lon.c_str());
        cJSON_AddItemToArray(list, item);
    }
    return SendJsonTree(req, root);
}

// ---- AI Q&A -------------------------------------------------------------------------------------

esp_err_t AiPageHandler(httpd_req_t* req) { return SendPage(req, kAiHtml); }

esp_err_t AiStateHandler(httpd_req_t* req) {
    auto& service = ai::ChatService::Get();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "started", service.Started());
    cJSON_AddBoolToObject(root, "hasKey", service.HasApiKey());  // the key itself is never sent back
    return SendJsonTree(req, root);
}

esp_err_t AiKeyHandler(httpd_req_t* req) {
    cJSON* body = ReadBody(req);
    if (!body) return SendError(req, "bad_json");
    cJSON* key = cJSON_GetObjectItemCaseSensitive(body, "key");
    const std::string value = cJSON_IsString(key) ? key->valuestring : "";
    cJSON_Delete(body);
    auto& service = ai::ChatService::Get();
    if (!service.SetApiKey(value)) {
        return SendError(req, "Key 无效，或设备尚未联网");
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    return SendJsonTree(req, root);
}

}  // namespace web
}  // namespace rawdraw
