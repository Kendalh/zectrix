# Project notes for Claude

ESP32-S3 4.2" four-color (BWRY) e-ink device. Firmware lives in `firmware/` (ESP-IDF, RawDraw UI).
The Python server described in README.md is **not in this repo** (only `server/mock_client.py`).
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
| `sim/fake/weather_api_fake.cc` (canned sample, built-in city table) | `common/weather_api.cc` (Open-Meteo HTTPS, task, NVS config) | network, TLS, NVS persistence, config loading |
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
  in its own task. `Application` starts it on Wi-Fi connect via `RawDrawUiManager::StartWeatherService()`.
  Up to 5 cities (`WeatherConfig`), UP/DOWN on the weather page calls `weather_api_select_city` (async; the sim
  fake answers synchronously, so `WeatherRenderer` sets its loading state *before* calling it). The sim has no network: `--demo-weather` (or saving a city at `/weather`) pushes a canned sample.
- Device web UI: `/` is the app-launcher home (`ui/renderers/rawdraw/web_apps.cc`), `/album` the photo page,
  `/weather` weather settings (`/api/weather`, `/api/weather/search`). New apps: add a card in `web_apps.cc`.
- `sim_device` runs the real `ApTransferServer` in LAN mode and keeps the album in memory
  (cleared on restart). The device HTTP API is documented in `docs/LAN_PHOTO_PUSH_API.md`. In the sim,
  "传图"/"配网" only show their instruction pages (no hotspot), so the device's conflict between the LAN
  service and AP mode is not reproduced.
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

- With the LAN service running (it auto-starts after Wi-Fi connects), opening the "传图" app shows hotspot
  instructions but `ApTransferServer::Start()` returns early ("already running"), and long-press BOOT then
  stops the LAN service via `StopApTransferMode()`. Pre-existing; the Home page made the entry more visible.

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
