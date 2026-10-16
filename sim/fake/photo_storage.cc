// In-memory replacement for common/photo_storage.cc (SPIFFS-backed on device).
// Same API and ordering semantics; the simulator seeds it with fixture photos.
#include "common/photo_storage.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
struct Stored {
    PhotoInfo info;
    std::vector<uint8_t> data;
};
std::vector<Stored> g_photos;
// The HTTP server thread and the render thread both touch the album.
std::mutex g_mutex;

Stored* Find(const char* id) {
    if (!id) return nullptr;
    for (auto& p : g_photos) {
        if (strcmp(p.info.id, id) == 0) return &p;
    }
    return nullptr;
}
}  // namespace

int photo_storage_init(void) { return 0; }

int photo_save(const PhotoInfo* info, const uint8_t* data) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!info || !data) return -1;
    Stored* existing = Find(info->id);
    Stored entry;
    entry.info = *info;
    entry.data.assign(data, data + info->file_size);
    if (existing) {
        *existing = std::move(entry);
    } else {
        g_photos.push_back(std::move(entry));
    }
    return 0;
}

int photo_load(const char* id, uint8_t* out, uint32_t max_size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Stored* p = Find(id);
    if (!p || !out) return -1;
    const uint32_t n = std::min<uint32_t>(max_size, static_cast<uint32_t>(p->data.size()));
    memcpy(out, p->data.data(), n);
    return static_cast<int>(n);
}

int photo_list(PhotoInfo* out, int max_count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const int n = std::min<int>(max_count, static_cast<int>(g_photos.size()));
    for (int i = 0; i < n; ++i) out[i] = g_photos[i].info;
    return n;
}

int photo_delete(const char* id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_photos.begin(); it != g_photos.end(); ++it) {
        if (strcmp(it->info.id, id) == 0) {
            g_photos.erase(it);
            return 0;
        }
    }
    return -1;
}

int photo_get_count(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_photos.size());
}

int photo_get_by_index(int index, PhotoInfo* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (index < 0 || index >= static_cast<int>(g_photos.size()) || !out) return -1;
    *out = g_photos[index].info;
    return 0;
}

bool photo_exists(const char* id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return Find(id) != nullptr;
}

int photo_update_info(const char* id, const PhotoInfo* updates) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Stored* p = Find(id);
    if (!p || !updates) return -1;
    strncpy(p->info.title, updates->title, sizeof(p->info.title) - 1);
    strncpy(p->info.date, updates->date, sizeof(p->info.date) - 1);
    strncpy(p->info.location, updates->location, sizeof(p->info.location) - 1);
    strncpy(p->info.body, updates->body, sizeof(p->info.body) - 1);
    return 0;
}

int photo_move(const char* id, int delta) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_photos.size(); ++i) {
        if (strcmp(g_photos[i].info.id, id) != 0) continue;
        const long j = static_cast<long>(i) + delta;
        if (j < 0 || j >= static_cast<long>(g_photos.size())) return -1;
        std::swap(g_photos[i], g_photos[j]);
        return 0;
    }
    return -1;
}
