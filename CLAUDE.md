# Project notes for Claude

ESP32-S3 4.2" four-color (BWRY) e-ink device. Firmware lives in `firmware/` (ESP-IDF, RawDraw UI).
The Python server described in README.md is **not in this repo** (only `server/mock_client.py`).
The firmware here has no WebSocket/voice client; it ships a Home page that launches the photo gallery,
weather, settings and the two hotspot pages, plus a LAN/AP HTTP upload service.

The dev machine cannot reach the device and has no ESP-IDF. Debug on the PC simulator in `sim/`;
the user builds and flashes the firmware elsewhere. User-facing behavior is documented in
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
  `weather_detail_renderer`, `settings_renderer`, `ap_transfer_renderer`
- `ui/app_registry.cc`, `ui/app_shell.cc`
- `ui/renderers/rawdraw/ap_transfer_server.cc` (upload/album HTTP API and embedded HTML)
- `components/78__xiaozhi-fonts/src/*.c` (fonts, except `cbin_font.c`)

Replaced in the simulator (firmware changes here are NOT exercised by `sim/`):

| Sim stand-in | Real firmware code | Not tested by sim |
| --- | --- | --- |
| `sim/sim_ui.cc` page switching + key routing | `ui/rawdraw_ui_manager.cc`, `application.cc` | status bar/frame, refresh, timers, slideshow, quick switch |
| `sim/sim_ui.cc` `BuildSettingsItems()` | settings list built in `application.cc` | real Wi-Fi/LAN/sleep actions; **keep labels and order in sync** |
| `sim/fake/photo_storage.cc` (in memory) | `common/photo_storage.cc` (SPIFFS + index) | capacity, index corruption, power loss |
| `sim/fake/weather_api.cc` (icon mapping only) | `common/weather_api.cc` (QWeather HTTP + JSON) | network, JSON parsing |
| `sim/sim_httpd.cc` (POSIX sockets) | ESP-IDF `esp_http_server` | timeouts, memory, real concurrency |
| `sim/sim_platform.c` font lookup | LVGL `lv_font_fmt_txt` | kerning is ignored, so spacing may differ by a few px |
| `sim/shim/*` (esp_log, esp_timer, freertos, wifi, netif, sleep, settings, wifi_manager, gpio) | real IDF and hardware | Wi-Fi/AP, DHCP, NVS, deep sleep, buttons, e-ink refresh |

Not compiled into the sim at all: `RawDrawUiManager`, `application.cc`, board code, audio, BLE
(`ble_image_receiver.cc`), the EPD refresh driver. Changes there need `idf.py build` or hardware.
`make -C sim check-firmware` type-checks `rawdraw_ui_manager.cc` only (stubs in `sim/check/`); it does
**not** cover `application.cc` and is not a substitute for `idf.py build`.

## Rules when changing code

1. Make the change in `firmware/main/`. Only touch `sim/` for stand-ins, shims, or sim tooling.
2. **Keep stand-ins in sync.** `sim/fake/weather_api.cc` duplicates `ParseWeatherIcon` from
   `firmware/main/common/weather_api.cc`; `sim_ui.cc` duplicates the manager glue and settings list (see
   tables above). If you change the signature or behavior of `photo_storage.h` / `weather_api.h`,
   update the matching fake.
3. **Shims only cover what is used today.** If firmware code starts using a new ESP-IDF/FreeRTOS/LVGL
   API in a file the sim compiles, add the minimal shim in `sim/shim/` (or sim_httpd/sim_platform)
   instead of ifdef-ing the firmware file. Do not add `#ifdef SIM` to firmware sources.
4. **Host build passing is not device build passing.** Xtensa gcc, real IDF headers and warning flags
   differ. Tell the user to run `idf.py build` (or `firmware/build.sh`) after a batch of changes;
   do not claim a change "works on the device" from sim results alone. New firmware `.cc` files must also
   be added to `firmware/main/CMakeLists.txt`.
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
- The device has no weather data source (`weather_api_init` is never called). The sim matches that: the
  weather page shows "暂无天气数据" unless started with `--demo-weather`.
- `sim_device` runs the real `ApTransferServer` in LAN mode and keeps the album in memory
  (cleared on restart). The device HTTP API is documented in `docs/LAN_PHOTO_PUSH_API.md`. In the sim,
  "传图"/"配网" only show their instruction pages (no hotspot), so the device's conflict between the LAN
  service and AP mode is not reproduced.
- Renderer `Init()` resets its state: feed data **after** `Init()`.
- Make flags are not dependencies: run `make -C sim clean` after changing `DEFS`/`INC` in the Makefile.
- Do not use `pkill -f`/`pgrep -f` with a pattern that appears in your own command line (it kills the
  shell). Use the `[x]` trick (`pgrep -af "[w]eb/server.py"`) and kill by pid.

## Known firmware issues (found by reading code; not fixed, not verified on hardware)

- With the LAN service running (it auto-starts after Wi-Fi connects), opening the "传图" app shows hotspot
  instructions but `ApTransferServer::Start()` returns early ("already running"), and long-press BOOT then
  stops the LAN service via `StopApTransferMode()`. Pre-existing; the Home page made the entry more visible.

## Known unresolved rendering oddities (sim output, cause unknown: shim vs firmware)

- `weather_detail`: left half (big temperature, city) is empty.
- `weather_detail`: feels-like value renders as `7°C` for `27`.
- `weather`: AQI number and its label ("38" / "优") overlap.

Check against `weather_detail_renderer.cc` / `weather_renderer.cc` and font `adv_w` before assuming either side is wrong.
(Seen with `--demo-weather` only now that the default weather page has no data.)

## Git

- Default branch of upstream is `2bp` (there is no `main`). Work on `feature`, created from `origin/2bp`.
- `origin` = `LazyYoun/youn-ink-fourcolor-firmware` (upstream; do not push).
- `kendalh` = the user's own repo `Kendalh/zectrix`. Push over SSH with the personal key alias:
  `git@github-personal:Kendalh/zectrix.git` (`~/.ssh/config`). The default SSH key and HTTPS login
  resolve to a different GitHub account and get 403/503.
- Commit/push only when the user asks; never create PRs (see global rules).
