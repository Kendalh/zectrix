// Host simulator: renders a rawdraw page into a 2bpp framebuffer and replays button events.
//   sim_main --page gallery|photo_detail|weather|weather_detail --out DIR [--keys up,down,boot,...]
// Writes DIR/<page>_<step>.fb after the initial render and after every key.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "sim_fixtures.h"
#include "ui/renderers/rawdraw/photo_detail_renderer.h"
#include "ui/renderers/rawdraw/photo_gallery.h"
#include "ui/renderers/rawdraw/weather_detail_renderer.h"
#include "ui/renderers/rawdraw/weather_renderer.h"

using namespace sim;

namespace {
void Dump(const std::string& dir, const std::string& page, int step, const std::vector<uint8_t>& fb) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%02d.fb", dir.c_str(), page.c_str(), step);
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fwrite(fb.data(), 1, fb.size(), f);
    fclose(f);
    printf("%s\n", path);
}
}  // namespace

int main(int argc, char** argv) {
    std::string page = "gallery", out = ".", keys;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--page")) page = argv[i + 1];
        else if (!strcmp(argv[i], "--out")) out = argv[i + 1];
        else if (!strcmp(argv[i], "--keys")) keys = argv[i + 1];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }

    SeedPhotos();
    // Init() resets renderer state, so data is injected afterwards (as the UI manager does on page entry).
    std::unique_ptr<rawdraw::PageRenderer> r;
    std::function<void()> feed = [] {};
    if (page == "gallery") {
        auto g = std::make_unique<rawdraw::PhotoGalleryRenderer>();
        feed = [p = g.get()] { p->RefreshPhotoList(); };
        r = std::move(g);
    } else if (page == "photo_detail") {
        r = std::make_unique<rawdraw::PhotoDetailRenderer>();
    } else if (page == "weather") {
        auto w = std::make_unique<rawdraw::WeatherRenderer>();
        feed = [p = w.get()] { p->Update(MakeWeather()); };
        r = std::move(w);
    } else if (page == "weather_detail") {
        auto w = std::make_unique<rawdraw::WeatherDetailRenderer>();
        feed = [p = w.get()] { p->Update(MakeWeather()); };
        r = std::move(w);
    } else {
        fprintf(stderr, "unknown page %s\n", page.c_str());
        return 2;
    }

    std::vector<uint8_t> fb(kBpr * kH, 0x55);
    r->Init(kW, kH);
    feed();
    r->Render(fb.data(), kW, kH);
    Dump(out, page, 0, fb);

    std::stringstream ss(keys);
    std::string k;
    int step = 1;
    while (std::getline(ss, k, ',')) {
        ButtonEvent ev;
        if (!ParseKey(k, &ev)) {
            fprintf(stderr, "unknown key %s\n", k.c_str());
            return 2;
        }
        const bool consumed = r->HandleInput(ev);
        r->Render(fb.data(), kW, kH);
        printf("key=%s consumed=%d\n", k.c_str(), consumed);
        Dump(out, page, step++, fb);
    }
    return 0;
}
