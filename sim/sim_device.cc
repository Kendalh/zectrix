// Long-running simulated device: the same UI code as the firmware (Home page, app shell, page
// renderers) plus the real ApTransferServer (the firmware's HTTP upload/album service) on a local port.
//
//   sim_device [--http-port 8080] [--host 127.0.0.1] [--seed-photos] [--demo-weather] [--serve]
//
// stdin commands (one per line), used by sim/web/server.py. Each answers with
// "FRAME <path-to-.fb> <last-key-consumed: 0|1|-1> <page>" or "ERR <reason>":
//   key <name>      press a key: up down boot, <name>long for long press, updownlong for the chord
//   screen          just render the current screen
//   jump <page>     debug: switch page directly (home gallery weather settings ap_transfer ...)
//   aistate         debug: AI sessions and the current conversation as a JSON line ("AISTATE {...}")
//   reset           like a reboot: back to the Home page
//   quit
// With --serve there is no command loop; the process just serves HTTP until killed.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <cJSON.h>

#include "common/ai_chat.h"
#include "sim_fixtures.h"
#include "sim_ui.h"

using namespace sim;

int main(int argc, char** argv) {
    bool serve_only = false, seed = false;
    SimUi::Options options;
    std::string host = "127.0.0.1", port = "8080", out_dir = "/tmp";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--serve")) serve_only = true;
        else if (!strcmp(argv[i], "--seed-photos")) seed = true;
        else if (!strcmp(argv[i], "--demo-weather")) options.demo_weather = true;
        else if (!strcmp(argv[i], "--http-port") && i + 1 < argc) port = argv[++i];
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    char voice_dir[] = "/tmp/sim_voice_XXXXXX";  // fake recordings; not cleaned up (tiny, in /tmp)
    if (mkdtemp(voice_dir)) options.voice_dir = voice_dir;
    setenv("SIM_HTTP_HOST", host.c_str(), 1);
    setenv("SIM_HTTP_PORT", port.c_str(), 1);
    options.lan_address = host + ":" + port;
    if (seed) SeedPhotos();

    SimUi ui(options);

    rawdraw::ApTransferServer server;
    server.SetStateCallback([&ui](rawdraw::ApTransferServer::ServerState s, const std::string& m) { ui.OnServerState(s, m); });
    server.SetImageReceivedCallback([&ui](const char*) { ui.OnImageReceived(); });
    server.SetPhotosChangedCallback([&ui]() { ui.OnPhotosChanged(); });
    server.SetSettingsChangedCallback([&ui](int minutes) { ui.OnSlideshowChanged(minutes); });
    server.SetShowPhotoCallback([&ui](const std::string& id) { return ui.ShowPhoto(id); });
    if (!server.StartLan(options.lan_address)) {
        fprintf(stderr, "failed to start HTTP server on %s\n", options.lan_address.c_str());
        return 1;
    }
    printf("READY http://%s/\n", options.lan_address.c_str());
    fflush(stdout);

    if (serve_only) {
        for (;;) pause();
    }

    int frame = 0;
    auto reply_frame = [&](int consumed) {
        std::vector<uint8_t> fb;
        ui.Render(&fb);
        char path[512];
        snprintf(path, sizeof(path), "%s/device_%d.fb", out_dir.c_str(), frame++ % 4);
        FILE* f = fopen(path, "wb");
        if (!f) { printf("ERR cannot write %s\n", path); return; }
        fwrite(fb.data(), 1, fb.size(), f);
        fclose(f);
        printf("FRAME %s %d %s\n", path, consumed, SimUi::PageName(ui.CurrentPage()));
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd, arg;
        in >> cmd >> arg;
        if (cmd == "quit") break;
        if (cmd == "key") {
            bool consumed = false;
            if (!ui.HandleKey(arg, &consumed)) printf("ERR unknown key\n");
            else reply_frame(consumed ? 1 : 0);
        } else if (cmd == "screen") {
            reply_frame(-1);
        } else if (cmd == "jump") {
            ui::RawDrawPageId page;
            if (!SimUi::PageFromName(arg, &page)) printf("ERR unknown page\n");
            else { ui.JumpTo(page); reply_frame(-1); }
        } else if (cmd == "aistate") {  // debug/test: AI sessions as one JSON line (no screen)
            auto& svc = ai::ChatService::Get();
            cJSON* root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "current", svc.CurrentSessionId());
            cJSON* sessions = cJSON_AddArrayToObject(root, "sessions");
            for (const auto& s : svc.List()) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", s.id);
                cJSON_AddStringToObject(item, "title", s.title.c_str());
                cJSON_AddNumberToObject(item, "count", static_cast<double>(s.message_count));
                cJSON_AddBoolToObject(item, "pending", s.pending);
                cJSON_AddItemToArray(sessions, item);
            }
            ai::Session cur;
            svc.Snapshot(svc.CurrentSessionId(), &cur);
            cJSON* messages = cJSON_AddArrayToObject(root, "messages");
            for (const auto& m : cur.messages) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "role", m.role == ai::Message::User ? "user" : "assistant");
                cJSON_AddStringToObject(item, "text", m.text.c_str());
                cJSON_AddItemToArray(messages, item);
            }
            cJSON_AddBoolToObject(root, "pending", cur.pending);
            cJSON_AddStringToObject(root, "error", cur.last_error.c_str());
            char* text = cJSON_PrintUnformatted(root);
            printf("AISTATE %s\n", text);
            cJSON_free(text);
            cJSON_Delete(root);
        } else if (cmd == "reset") {
            ui.Reboot();
            reply_frame(-1);
        } else {
            printf("ERR unknown command\n");
        }
        fflush(stdout);
    }
    server.Stop();
    return 0;
}
