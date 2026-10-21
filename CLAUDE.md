# Project notes for Claude

ESP32-S3 4.2" four-color (BWRY) e-ink device. Firmware lives in `firmware/` (ESP-IDF, RawDraw UI).
There is no Python server in this repo (only `server/mock_client.py`, an old protocol debug script).
The firmware here has no WebSocket/voice client; it ships a Home page that launches the photo gallery,
weather, settings and the two hotspot pages, plus a LAN/AP HTTP upload service.

The dev machine cannot reach the device. Debug on the PC simulator in `sim/`. ESP-IDF v6.0.0 is installed
here (see "Building the firmware"), so `.bin` files are built here, published for download (see "Publishing a
build"), and flashed by the user on another machine. Nothing has been run on a real device yet. User-facing behavior is documented in
`user_manual.md`: keep it in sync when adding pages, buttons, apps or settings entries.

## App navigation (shared between firmware and simulator)

The device boots to the **Home page**: a list of apps. UP/DOWN select, BOOT opens, **long-press BOOT
returns Home from any page**. UP+DOWN long press starts Wi-Fi config. UP/DOWN long press are passed to
the active page (Settings: jump to top/bottom). There are no page-switching shortcuts other than these.

- `ui/app_registry.{h,cc}`: the app list (`GetApps()`). **To add an app, add one entry here**; Home, the
  firmware and the simulator pick it up. `AppAction`: `OpenPage`, `StartApTransfer`, `StartWifiConfig`.
- `ui/app_shell.{h,cc}`: hardware-free rules (`Launch`, `GoHome`, `HandleGlobalInput`). The platform
  supplies `Host` callbacks (open page, start/stop hotspot, start Wi-Fi config).
- `ui/renderers/rawdraw/home_renderer.{h,cc}`: the Home page. Keeps its selection across `Init()`.
- `ui/page_ids.h`: `RawDrawPageId` (moved out of `rawdraw_ui_manager.h` so the sim can include it).
- Firmware glue: `RawDrawUiManager` (`app_shell_`, `home_renderer_`, BOOT-long block in `HandleInput`,
  `StopApTransferMode` -> Home) and `Application` (UP/DOWN long press forwarded to the page,
  Wi-Fi events return to Home, `SetWifiConfigRequestCallback`).
- Simulator glue: `sim/sim_ui.{h,cc}` mirrors the manager's page switching and key routing.

**Whenever you change navigation glue in `RawDrawUiManager`/`Application`, make the same change in
`sim/sim_ui.cc`** (and the other way round), then run `make -C sim test`. Decisions belong in
`AppShell`/`HomeRenderer` where possible so there is only one copy.

Voice app (`RawDrawPageId::Voice`): `ui/renderers/rawdraw/voice_renderer.*` (shared with the sim) talks to
`common/voice_backend.h` (interface) and `common/voice_recordings.*` (WAV naming/listing, shared). Firmware backend:
`voice/esp_voice_backend.*` (PCM tap `AudioService::SetPcmSink`, playback via `PushPcmToPlaybackQueue`); sim fake:
`sim/fake/voice_backend_fake.*`. Hold UP = `kUpLongPress` (at 1 s) ... release = `kUpLongRelease`, sent by the board's
UP `OnPressUp` -> `Application::OnUpLongRelease` (web sim key `uplongrelease`). `RawDrawUiManager::SwitchPage` and
`SimUi::SwitchPage` call `VoiceRenderer::OnLeave()` when leaving the page. Not run on hardware.

AI Q&A app (`RawDrawPageId::AiChat`, Alibaba Bailian/DashScope Qwen, non-streaming, **voice input only**): logic is in
`common/ai_chat.{h,cc}` (`ai::ChatService` singleton: sessions, per-session context trimmed to `kMaxContextChars`, one request in
flight, failed questions are dropped from the context; pure helpers `BuildRequestJson`/`ParseResponse`/`TrimHistory`/
`SpeechBodyParts`/`IsUsableTranscript`/`Base64Encode`, unit-tested by `sim/tests/test_ai_chat.cc`). One API key and one endpoint
(`dashscope.aliyuncs.com/compatible-mode/v1/chat/completions`) serve both models: chat `qwen-flash`, speech `qwen3-asr-flash`
(an `input_audio` part with a base64 `data:audio/wav` URL; the transcript is the assistant `content`; silence comes back as a lone
"嗯。", which `IsUsableTranscript` rejects). Input: hold UP = `kUpLongPress` starts the Voice app's `VoiceBackend`, release
(`kUpLongRelease`) stops it; the backend's `SetOnSaved` hook hands the WAV to `AiChatRenderer::OnRecordingSaved` (claims it, so it
never reaches the Voice list) and `ChatService::AskVoice` sends it for recognition, then the text is asked like a question.
Sessions are managed on the device (BOOT = session list, DOWN long = new/delete); the web page `/ai` (`web_apps.cc`,
`/api/ai/key|state`) only stores the key, there is deliberately no web question input. Hardware sits behind `ai::ChatPlatform` (one
generic `Send(HttpRequest)` with an optional `body_file` that the platform turns into prefix+base64+suffix via `ai::BuildFileBody`
on its own task, because the voice save task has a 4 KB stack, plus key storage): firmware `common/ai_chat_platform_esp.cc` (one
short task per request, 10 KB stack, 90 s timeout, key in NVS namespace `aichat` slot `bl_key`), sim
`sim/fake/ai_chat_platform_fake.cc` (system `curl`, key from `DASHSCOPE_API_KEY`; `SIM_AI_FAKE=1` = canned transcript/answers,
`SIM_AI_SPEECH_TEXT` overrides the transcript; `SIM_VOICE_WAV=file.wav` makes the fake recorder "record" that file). The sim_device
command `aistate` dumps the sessions for tests. `AiChatRenderer::NeedsRefreshAfterChange()` suppresses the panel refresh while a
question is pending (15 s refresh, no "thinking" screen). `AiChatRenderer::OnLeave()` runs from
`RawDrawUiManager::SwitchPage`/`SimUi::SwitchPage`. `Application` wires `voice_backend_->SetOnSaved`,
`RawDrawUiManager::StartAiChatService()` is called on Wi-Fi connect. The key is never in the source or repo. Verified against the
real Bailian endpoint from the sim (TTS speech -> ASR -> chat); the device-side TLS/NVS/memory path was never run on hardware.

AI painter app (`RawDrawPageId::AiPaint`, Home entry "AI 画图"; same Bailian key and `ai::ChatPlatform` as AI Q&A, no web page): logic in
`common/ai_paint.{h,cc}` (`ai::paint::PaintService` singleton: one job at a time, phases Transcribing -> Planning -> Generating -> Downloading ->
Converting -> Ready/Error; pure helpers `NormalizeSubject`/`StylePrompt`/`BuildImageRequestJson`/`ParseImageResponse`/`PngToBwry`) and
`common/png_decode.{h,cc}` (own inflate + 8-bit PNG decoder: ESP-IDF has no zlib and LVGL's lodepng allocates via `lv_malloc`), both
unit-tested by `sim/tests/test_ai_paint.cc` (fixtures in `sim/tests/data/`, made with PIL). Flow: hold UP records through the Voice
backend, `AiPaintRenderer::OnRecordingSaved` claims the WAV (the two AI pages' claims are chained in `Application` and `SimUi`),
`qwen3-asr-flash` transcribes, then a planning call to `qwen-flash` with the system prompt `kPlannerSystem` turns the subject into one English scene
description (it picks the sky and the celestial disc from the time of day/weather in the words: sunset = red sun (default), day = yellow sun, night =
moon, rain/snow/fog = none; do NOT hard-code a sun in the shared style), which goes into `kPosterPrefix`/`kPosterSuffix` (the Pudong/Fuji poster look,
`kNegativePrompt`); if planning fails `FallbackPrompt` (subject in a neutral template) is used. `qwen-image-2.0-pro` (sync `multimodal-generation` endpoint; `qwen-image-3.0-pro` and `qwen-image-plus` do not work on
`dashscope.aliyuncs.com`) returns a pre-signed PNG URL (640x480: the API rejects images under 512x512 pixels, so the panel's 400x300 is not allowed), the PNG is downloaded with a GET (no Authorization header; `HttpRequest::max_response`
raised to 4 MB, `buffer_size_tx` 2048 for the long URL) and `PngToBwry` crops 3%, area-averages to 400x300 and Floyd-Steinberg dithers onto
black/white/yellow/red (2 bpp codes 00/01/10/11, the `rawdraw::Color` order). Network-level failures (no HTTP answer) of the image request and the download are retried once (`SendWithRetry`, 1.5 s apart, on the request task); a download over
TLS that fails once goes straight to plain `http://` with the same pre-signed URL (`StartDownload`; on the device the OSS accelerate host fails certificate
verification, "No matching trusted root certificate found", with the FULL Mozilla bundle, so HTTPS is not retried first); the planner and the speech request are never retried. The picture is held in RAM; UP = redraw (`Regenerate`), DOWN =
`SaveToAlbum` (once per picture, `photo_save`, 30000-byte 2 bpp photo). `AiPaintRenderer::IsFullscreenMode()` makes `RawDrawUiManager::RenderAll`
hide the status bar/frame while a picture is shown; the subject ("画: …") is shown only with the finished picture, never on the working page
(`Status::subject` is still the previous one while working). Like AI Q&A it refreshes the panel only when painting starts and ends
(`NeedsRefreshAfterChange` compares a coarse signature). Sim: `fake/ai_chat_platform_fake.cc` answers the image request and serves a synthetic
PNG in `SIM_AI_FAKE=1` mode (`SIM_AI_IMAGE_PNG=file.png` overrides it), `sim_device` commands `paint TEXT` (skips the recording; real model
call when `SIM_AI_FAKE` is unset and `DASHSCOPE_API_KEY` is set) and `paintstate`. Verified against the real endpoint from the sim (screens
look right); **not compiled for the device and never run on hardware**: TLS download of ~1 MB, PSRAM use while decoding, and doing the
conversion on the 10 KB request task (a long CPU burst; task watchdog) are unverified.

Token board app (`RawDrawPageId::TokenBoard`, Home entry "Token 看板"): Alibaba Bailian balance and cost from the Alibaba Cloud BSS OpenAPI.
Logic in `common/token_board.{h,cc}` (`board::BoardService` singleton, hardware-free, unit-tested by `sim/tests/test_token_board.cc`):
RPC signature v1 (own SHA1/HMAC-SHA1, GET with a signed query to `business.aliyuncs.com`), `QueryAccountBalance`, then
`DescribeInstanceBill` DAILY for yesterday and today (rows with `ProductCode` `sfm`, summed per model from `InstanceID`, `ModelFromInstanceId`);
one request in flight, chained balance -> yesterday -> today, nothing in the background: `Refresh(false)` runs when the page is entered
(`RawDrawUiManager::InitRenderer`/`SimUi::SwitchPage`; stale after 120 s), BOOT = `Refresh(true)`. **The bill carries money only, no token counts**
(`Usage` is empty), and there is no hourly bill: `Granularity=HOURLY` ignores `BillingDate` and returns the month so far, and today's daily bill lags.
So today/this-hour cost are estimates from the balance (first balance seen in the Beijing day/hour is the baseline, kept in NVS key `base`; a top-up
resets it; today = max(baseline - balance, today's bill total)). Hardware behind `board::Platform` (generic `Send` = the AI chat platform's, plus
`Load/Save` and `Now`): firmware `common/token_board_platform_esp.cc` (NVS namespace `tokenboard`: `ak_id`, `ak_secret`, `base`), sim
`fake/token_board_platform_fake.cc` (real mode = curl to the real endpoint with `ALIBABA_CLOUD_ACCESS_KEY_ID`/`_SECRET`; `SIM_BOARD_FAKE=1` = canned
responses, balance drops 0.05 per refresh). Renderer `token_board_renderer.*` (header pill with the provider name + page dots; hero: balance in 2x scaled text on black
(`DrawTextScaled`, the only fonts are 16 px; falls back to 1x when it does not fit), today and this hour as 1x text on the right; two cards below, yesterday / today by model with a
yellow proportion bar per row; `NeedsRefreshAfterChange` compares content so the start of a refresh does not redraw). One provider per page,
`board::Providers()` lists them (阿里百炼, DeepSeek); UP/DOWN page through them (and refresh the newly shown one if stale), a new provider needs an
entry there, its own service and a render branch. Layout: top half = totals (`RenderHero`), bottom half = detail cards (`RenderRowsCard`, top 3 rows).
DeepSeek (`board::DeepSeekService`, NVS `ds_key`/`ds_base`, web `POST /api/tokenboard/deepseek`, sim env `DEEPSEEK_API_KEY`, sim command `dsstate`): only
`GET https://api.deepseek.com/user/balance` (Bearer API key); DeepSeek has no usage/bill API an API key can call (the console's `platform.deepseek.com/api/v0/usage/*`
needs a login session token, not used), so its detail card shows the balance parts (topped-up, granted); today/hour are balance estimates (`board::Baselines`, shared
with Bailian). Never add the three balance fields (`total_balance` already includes the others). Not run against the real DeepSeek endpoint yet. Refresh interval (10/30/60 min, default 60, NVS key `interval`, `POST /api/tokenboard/interval`) is the
validity of the data when the page is entered, not a polling period (same rule as weather: no background polling). Web page `/tokenboard` + `/api/tokenboard/state|key`
(`web_apps.cc`; write-only, the secret is never returned; `max_uri_handlers` raised to 40). The AccessKey should be a RAM sub-account with
`AliyunBSSReadOnlyAccess`; it is stored in NVS in plain text. China site only. Sim command `boardstate`. Verified against the real BSS endpoint from
the sim; **not compiled for the device, never run on hardware** (TLS, NVS, SNTP clock, the 48 KB response cap = ~40 bill rows are unverified).

Calendar app (`RawDrawPageId::CalendarApp`, Home entry "日历"; the older `Calendar` page/`CalendarRenderer` is a different,
unreachable lunar calendar): `ui/renderers/rawdraw/calendar_app_renderer.*` (month grid, bell icon drawn from a 9x9 bitmap on dates with
reminders, "today" list view; UP/DOWN = month, BOOT = back to current month, or today's list when already there) reads
`common/reminders.{h,cc}` (hardware-free: date helpers, `Store` singleton `Store::Shared()` with limits, JSON file `Persistence`,
unit-tested by `sim/tests/test_reminders.cc`). Both firmware and sim persist to a plain file via `NewFilePersistence` (firmware
`/spiffs/reminders.json` set up in the `RawDrawUiManager` constructor, which also redraws the page on `SetOnChange`; sim
`--reminders-file`, default `/tmp/sim_reminders.json`). `sim_device --today YYYY-MM-DD` fixes the calendar's clock (tests use it). Web page
The bottom notice line comes from `common/notice_api.{h,cc}`: its task is idle and fetches **only when the calendar app is entered**
(`notice_api_refresh_if_stale()` from `RawDrawUiManager::InitRenderer`/`SimUi::SwitchPage`; never at boot/Wi-Fi connect, no timer; the 10/30/60 min interval is
the validity of the last fetch, 0 = never fetch; `sim_device noticestate` counts fetches for `NoticeOnDemandTest`). Web page
`/calendar` + `GET/POST /api/reminders` (`web_apps.cc`; POST replaces one date's list). `StartLanHttpServer` hands the address to the page.
Colors in `CalendarAppRenderer` are the raw `rawdraw::Color` values on purpose (white page; black weekday band with yellow Sat/Sun letters; red
weekend numbers; yellow festival names; yellow block with black text = today; bell red, black on today), not theme tokens; the web grid
(`kCalendarHtml`) mirrors them. Keep both in sync when changing.
Festival names are drawn with `font_festival_12` (12 px, 1 bpp subset of Source Han Sans SC holding only the characters of the names in
`chinese_calendar.cc`; `sim/tools/gen_festival_font.sh FONT.otf` regenerates it, rerun it when a name gets a new character, otherwise that
character renders blank). Zpix was rejected: its license forbids converting/splitting the font.
Chinese festivals (2010-2050 only; nothing outside) come from `common/chinese_calendar.{h,cc}` (`FestivalsOn`/`MainFestival`, `ToLunar`) over the generated
`common/lunar_table.inc` (`sim/tools/gen_lunar_table.py` with the pip package `lunar_python`; do not use `Calendar::ToLunarDate` in
`rawdraw/components/calendar.cc`, it assumes alternating 30/29-day months). Verified day by day against the package (14975 days, 0 mismatches):
`LUNAR_DAYS_CSV=file sim/build/test_chinese_calendar` with the CSV from the script's `--dump-days`; plain `make -C sim test` only runs the
known-date checks. The web page gets them from `GET /api/festivals?year=`. No statutory holiday/make-up-workday schedule.
Not run on hardware: SPIFFS write path, the system clock being synced, a four-color panel's red "today" highlight on the B/W Note 4.

Recipes app (`RawDrawPageId::Recipes`, Home entry "菜谱"): 10 built-in recipes from HowToCook (Unlicense), no network, no service.
Data: `sim/tools/recipes/recipes.json` (hand-condensed text: every step one 16 px line, intro <= 3 lines, page 2 <= 300 px) and `recipes/photos/<id>.jpg`
(360x278 center crops; `gen_recipes.py --import-photos HOWTOCOOK_DIR` re-imports them) -> `python3 -I sim/tools/gen_recipes.py` writes
`common/recipes_data.inc` (const arrays in flash, ~90 KB; photos 180x139 2 bpp BWRY, Floyd-Steinberg, same codes as `rawdraw::Color`) which only
`common/recipes.cc` includes (`recipes::Count()/Get(i)`). Renderer `ui/renderers/rawdraw/recipe_renderer.*` (shared with the sim): list mode
(like Home, status bar drawn by the manager) and a full-screen detail mode (`IsFullscreenMode()` -> `RawDrawUiManager::RenderAll` hides the status
bar/frame, like the AI painter's picture): page 1 photo/stars/kcal/intro, page 2 ingredients + steps (+ yellow tip bar when it fits). DOWN after page 2
goes on to the next recipe's page 1 (UP the reverse), BOOT = back to the list. UP/DOWN on this page are in the manager's `cursor_click` set (coalesced,
one refresh ~800 ms after the last press). Fonts: Regular 16 px for body, Medium **24 px** (not 16) for titles; stars are a 11x11 bitmap, no glyphs.
`sim/tests/test_recipes.cc` checks every character against the real font (`SourceHanSansSC_*_slim` are subsets: a missing glyph renders blank), one-line
steps and the 300 px budget, so rerun `make -C sim test` after editing the JSON. `sim_main --page recipes --keys boot,down` renders the pages.
Not compiled for the device (`idf.py build` not run); the data file is plain const arrays, so the likely device-only risks are the link size (+~90 KB) and
how the dithered photos look on the real panel.

Gotchas found while building this (keep in mind for new pages):
- Entering a page runs the renderer's `Init()`, which resets it (gallery: card mode, first photo). Anything
  that "selects then switches" is broken; switch first, then select (see `ShowPhotoById`).
- `weather_icons.h` declares fonts with C linkage and clashes with `rawdraw/font_engine.h`: do not include
  both in one TU. `fa_settings.h` pulls in real `<lvgl.h>` and must come before `font_engine.h`.
- `FA_SETTINGS_LINK` is U+F301 and is **not in the font** (renders blank). Check glyphs exist before use.

## Simulator (`sim/`) vs firmware: what is shared

`sim/Makefile` compiles firmware sources **in place from `firmware/main/`** (no copies). Edit the
firmware file and `make -C sim` picks it up; the same edit ships to the device. Never fork a
firmware file into `sim/`.

Shared (compiled unmodified, fixes apply to both):
- `firmware/main/rawdraw/*` and `rawdraw/components/*` (except `calendar.cc`, needs `holiday_fetcher.h`)
- renderers: `home_renderer`, `photo_gallery`, `photo_detail_renderer`, `weather_renderer`,
  `weather_detail_renderer`, `settings_renderer`, `ap_transfer_renderer`, `voice_renderer`, `ai_chat_renderer`, `ai_paint_renderer`
- `common/ai_chat.cc`, `common/ai_paint.cc`, `common/png_decode.cc`, `common/weather_parse.cc`, `common/voice_recordings.cc`
- `ui/app_registry.cc`, `ui/app_shell.cc`
- `ui/renderers/rawdraw/ap_transfer_server.cc` (upload/album HTTP API and embedded HTML)
- `components/78__xiaozhi-fonts/src/*.c` (fonts, except `cbin_font.c`)

Replaced in the simulator (firmware changes here are NOT exercised by `sim/`):

| Sim stand-in | Real firmware code | Not tested by sim |
| --- | --- | --- |
| `sim/sim_ui.cc` page switching + key routing | `ui/rawdraw_ui_manager.cc`, `application.cc` | status bar/frame, refresh, timers, slideshow, quick switch |
| `sim/sim_ui.cc` `BuildSettingsItems()` | settings list built in `application.cc` | real Wi-Fi/LAN/sleep actions; **keep labels and order in sync** |
| `sim/fake/photo_storage.cc` (in memory) | `common/photo_storage.cc` (SPIFFS + index) | capacity, index corruption, power loss |
| `sim/fake/weather_api_fake.cc` (canned sample, built-in city table) | `common/weather_api.cc` (Open-Meteo HTTPS, task, NVS config) | network, TLS, NVS persistence, config loading |
| `sim/fake/ai_chat_platform_fake.cc` (`curl` or canned replies, key in memory) | `common/ai_chat_platform_esp.cc` (esp_http_client TLS, NVS key) | TLS, NVS, task stack, 90 s timeout on a real network |
| `sim/sim_httpd.cc` (POSIX sockets) | ESP-IDF `esp_http_server` | timeouts, memory, real concurrency |
| `sim/sim_platform.c` font lookup | LVGL `lv_font_fmt_txt` | kerning is ignored, so spacing may differ by a few px |
| `sim/shim/*` (esp_log, esp_timer, freertos, wifi, netif, sleep, settings, wifi_manager, gpio) | real IDF and hardware | Wi-Fi/AP, DHCP, NVS, deep sleep, buttons, e-ink refresh |

Not compiled into the sim at all: `RawDrawUiManager`, `application.cc`, board code, audio, BLE
(`ble_image_receiver.cc`), the EPD refresh driver. Changes there need `idf.py build` or hardware.
`make -C sim check-firmware` type-checks `rawdraw_ui_manager.cc` only (stubs in `sim/check/`); it does
**not** cover `application.cc` and is not a substitute for `idf.py build`.

## Rules when changing code

1. Make the change in `firmware/main/`. Only touch `sim/` for stand-ins, shims, or sim tooling.
2. **Keep stand-ins in sync.** `sim/fake/weather_api_fake.cc` implements the `weather_api.h` interface
   (parsing/validation in `common/weather_parse.cc` is shared, not duplicated); `sim_ui.cc` duplicates the manager glue and settings list (see
   tables above). If you change the signature or behavior of `photo_storage.h` / `weather_api.h`,
   update the matching fake.
3. **Shims only cover what is used today.** If firmware code starts using a new ESP-IDF/FreeRTOS/LVGL
   API in a file the sim compiles, add the minimal shim in `sim/shim/` (or sim_httpd/sim_platform)
   instead of ifdef-ing the firmware file. Do not add `#ifdef SIM` to firmware sources.
4. **Host build passing is not device build passing.** Xtensa gcc, real IDF headers and warning flags
   differ. **Do not run `firmware/scripts/build_local.sh` (or `idf.py build`) after every change: a build is
   slow. Build only when the user asks for it** (see "Building the firmware"); the same goes for uploading a
   build. Without a build, say that the change was not compiled for the device. Do not claim a change "works on
   the device" from sim or build results alone. New firmware `.cc` files must also be added to
   `firmware/main/CMakeLists.txt`.
5. Separate pure logic/rendering from hardware calls and put hardware behind a thin interface, so the
   logic is shared and the sim supplies a fake for the hardware side. Apply this to new apps.
6. When reporting results, say which part was verified in the sim and which still needs real hardware.
7. Run `make -C sim test` (navigation tests + web button-logic test) and `make -C sim check-firmware`
   after changes to navigation, the app registry, or `rawdraw_ui_manager.*`.

## Running the simulator

```bash
make -C sim                                   # builds sim_main + sim_device; fetches cJSON v1.7.18 once into sim/build/
make -C sim test                              # navigation tests (python) + button logic (node, skipped if missing)
make -C sim check-firmware                    # type-check rawdraw_ui_manager.cc against stubs
make -C sim run PAGE=weather KEYS=down,boot   # one-shot renderer only, no Home/shell: PNGs in sim/out/
python3 sim/web/server.py                     # web UI http://127.0.0.1:8000, device upload service http://127.0.0.1:8080
```

- `sim_device` is stateful: the web UI sends each key once (`/key?name=`, `/screen`, `/jump?page=`,
  `/reset`; headers `X-Page`, `X-Consumed`). Do not replay key history: replaying would re-trigger side
  effects like starting a hotspot. Key names: `up down boot`, `<name>long`, `updownlong` (the UP+DOWN chord).
- Long press is 1000 ms (`kNavLongPressMs` in `boards/zectrix-s3-epaper-4.2/zectrix-s3-epaper-4.2.cc`,
  verified). **The real board never registers double-click**, so features that depend on `k*DoubleClick`
  events (photo delete dialog, quick-switch overlay) cannot be reached on the device; the sim has no
  double-click either.
- Weather: data source is Open-Meteo (no key). `common/weather_parse.cc` (shared with the sim, unit-tested by
  `sim/tests/test_weather_parse.cc` in `make -C sim test`) parses it; `weather_api.cc` (firmware only) does HTTPS
  in its own task. `Application` creates the (idle) task on Wi-Fi connect via `RawDrawUiManager::StartWeatherService()`; **it fetches nothing then and has no timer**: weather is
  fetched only on request (`weather_api_refresh_if_stale()` from `RawDrawUiManager::InitRenderer`/`SimUi::SwitchPage` when the Weather page is entered, the
  refresh key, a city switch, saving the web settings). `interval_min` is the cache validity, not a polling period. Do not add background polling.
  Up to 5 cities (`WeatherConfig`), UP/DOWN on the weather page calls `weather_api_select_city` (async; the sim
  fake answers synchronously, so `WeatherRenderer` sets its loading state *before* calling it). The sim has no network: `--demo-weather` (or saving a city at `/weather`) pushes a canned sample.
- Device web UI: `/` is the app-launcher home (`ui/renderers/rawdraw/web_apps.cc`), `/album` the photo page,
  `/weather` weather settings (`/api/weather`, `/api/weather/search`). New apps: add a card in `web_apps.cc`.
- `sim_device` runs the real `ApTransferServer` in LAN mode and keeps the album in memory
  (cleared on restart). The device HTTP API is documented in `docs/LAN_PHOTO_PUSH_API.md`. In the sim,
  "配网" only shows its instruction page (no hotspot). The "传图" app was removed from Home (the web UI covers it);
  `AppAction::StartApTransfer` and `APTransfer` page code remain for the Wi-Fi config page.
- Renderer `Init()` resets its state: feed data **after** `Init()`.
- Make flags are not dependencies: run `make -C sim clean` after changing `DEFS`/`INC` in the Makefile.
- Do not use `pkill -f`/`pgrep -f` with a pattern that appears in your own command line (it kills the
  shell). Use the `[x]` trick (`pgrep -af "[w]eb/server.py"`) and kill by pid.

## Building the firmware (.bin)

The target device (Zectrix Note 4C: ESP32-S3, 16 MB flash, 8 MB PSRAM, 4.2" 400x300 four-color) is the
`zectrix-s3-epaper-4.2` board in this repo. This machine has ESP-IDF installed (built here, flashed elsewhere):

```bash
firmware/scripts/build_local.sh            # incremental; add --clean for a from-scratch build
# -> firmware/releases/v<PROJECT_VER>_zectrix-s3-epaper-4.2/  (gitignored)
#      merged-binary.bin     full image, flash at 0x0
#      app-only_xiaozhi.bin  app only, flash at 0x20000 (keeps NVS/settings)
#      SHA256SUMS, BUILD_INFO.txt
```

- **ESP-IDF must be v6.0.0** (`~/esp/esp-idf`, tag `v6.0`), the version recorded in `firmware/dependencies.lock`.
  v6.0.3 fails: the locked `esp_cam_sensor` (an unused leftover dependency) calls SPI internals that changed in
  a 6.0.x patch release. The script refuses to run on a different version (`FORCE=1` overrides). Do not "fix"
  it by removing dependencies from `idf_component.yml`.
- `build.sh` and `scripts/release.py` cannot be used: they need `main/boards/zectrix-s3-epaper-4.2/config.json`,
  which this repo does not ship (and `build.sh` also syncs into a `../main/manager-next` that does not exist
  here). `build_local.sh` reproduces what `release.py` does: `set-target esp32s3`, append
  `CONFIG_BOARD_TYPE_ZECTRIX_S3_EPAPER_4_2=y`, `idf.py build`, `idf.py merge-bin`.
- Environment details the script handles: system Python first in `PATH` (a project venv earlier in `PATH`
  breaks IDF's own Python env); a private libusb (`~/esp/libusb-pkg/extracted/...`, via `LD_LIBRARY_PATH`)
  because `export.sh` insists on openocd, which needs `libusb-1.0` and there is no root here.
- Tools live in `~/.espressif`. If the install is lost: `git clone --depth 1 --branch v6.0 --recursive
  --shallow-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf`, then with the system Python
  `python3 tools/idf_tools.py install xtensa-esp-elf riscv32-esp-elf esp32ulp-elf xtensa-esp-elf-gdb cmake ninja
  esp-rom-elfs openocd-esp32` (set `LD_LIBRARY_PATH` to a libusb extracted with `apt-get download
  libusb-1.0-0 && dpkg -x`) and `python3 tools/idf_tools.py install-python-env`. `install.sh` alone aborts at
  openocd and leaves CMake/Ninja/the Python env uninstalled.
- The first build downloads the managed components from components.espressif.com into `firmware/managed_components/`
  (gitignored) and compiles ~2500 objects. `build/`, `sdkconfig`, `managed_components/` are all untracked;
  `dependencies.lock` **is** tracked, and the script warns if the build changed it (it should not with v6.0.0).
- The image does not include the 8 MB `assets` SPIFFS partition (0x800000, photo storage).
- Flash command (device must be in download mode; the procedure is not documented anywhere I found):
  `python -m esptool --chip esp32s3 -b 460800 write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 merged-binary.bin`
- A successful build proves it compiles and links; nothing has been flashed or run on a real device.

## Publishing a build for download (Alibaba Cloud nginx)

The user downloads the `.bin` on another machine, so publish it with:

```bash
firmware/scripts/upload_firmware.sh        # uploads the newest firmware/releases/v*/merged-binary.bin
```

- Result: `/var/www/aifin/firmware/merged-binary_<YYYYMMDD-HHMM>.bin` plus `<name>.sha256`, linked as
  `https://<domain>/fin/firmware/<name>`. **Put files directly in `/fin/firmware/`, no random sub-directories**
  (user decision). The timestamp is the file's mtime (when `merge-bin` produced it) in **UTC**; the script never
  overwrites an existing file, so a link that was handed out keeps pointing at the same bytes.
- Server, key and directory are **not** stored in this repo (it is pushed to GitHub). The script reads them from
  `~/workspace/aifin/sync_reports_to_cloud.sh`; the setup is documented in `~/workspace/aifin/CLAUDE.md`
  ("云端部署"). The same host serves the aifin weekly reports and a Flask app: only ever add files under
  `/var/www/aifin/firmware/`, never edit the nginx config (`/fin/` already aliases `/var/www/aifin/`,
  `autoindex off`), and never touch `index.html`/`report/` (aifin's sync script owns them and `--delete`s there).
- **This dev machine cannot download the public link**: its network's web filter answers 403 "Access Denied"
  (category "Finance;Suspicious") for the domain, even for aifin's existing pages. Do not try to get around it.
  The script instead verifies on the server through nginx (loopback + `Host` header) and compares SHA-256.
- The site's TLS certificate expired on 2026-04-04, so browsers warn and `curl` needs `-k`. Always give the user
  the SHA-256 in the chat message, and tell them to verify the download against it.
- Everything published is world-readable (no auth). Delete a build only when asked, naming the exact file:
  `ssh -i ~/.ssh/aifin_deploy <host> 'rm -- /var/www/aifin/firmware/<name>.bin /var/www/aifin/firmware/<name>.bin.sha256'`.
- Report the link, the SHA-256, and that it was verified on the server (not from the public internet).

## Known firmware issues (found by reading code; not fixed, not verified on hardware)

- The Home page no longer has a "传图" (hotspot upload) entry; the Wi-Fi config page still uses `ApTransferServer`
  (`StopApTransferMode()` also stops the LAN service when leaving it).

## Known unresolved rendering oddities (sim output, cause unknown: shim vs firmware)

- `weather_detail`: left half (big temperature, city) is empty.
- `weather_detail`: feels-like value renders as `7°C` for `27`.
- `weather`: AQI number and its label ("38" / "优") overlap.

Check against `weather_detail_renderer.cc` / `weather_renderer.cc` and font `adv_w` before assuming either side is wrong.


## Git

- Default branch of upstream is `2bp` (there is no `main`). Work on `feature`, created from `origin/2bp`.
- `origin` = `LazyYoun/youn-ink-fourcolor-firmware` (upstream; do not push).
- `kendalh` = the user's own repo `Kendalh/zectrix`. Push over SSH with the personal key alias:
  `git@github-personal:Kendalh/zectrix.git` (`~/.ssh/config`). The default SSH key and HTTPS login
  resolve to a different GitHub account and get 403/503.
- Commit/push only when the user asks; never create PRs (see global rules).
