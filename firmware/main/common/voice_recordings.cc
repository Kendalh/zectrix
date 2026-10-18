#include "common/voice_recordings.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace voice {

namespace {

// "rec_0007.wav" -> 7, anything else -> -1
int ParseNumber(const char* name) {
    int n = -1;
    char tail[8] = {0};
    if (sscanf(name, "rec_%d.%7s", &n, tail) == 2 && strcmp(tail, "wav") == 0 && n >= 0) return n;
    return -1;
}

void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

}  // namespace

std::vector<uint8_t> MakeWavHeader(uint32_t data_bytes) {
    std::vector<uint8_t> h;
    h.reserve(kWavHeaderBytes);
    h.insert(h.end(), {'R', 'I', 'F', 'F'});
    Put32(h, 36 + data_bytes);
    h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    Put32(h, 16);
    Put16(h, 1);                 // PCM
    Put16(h, 1);                 // mono
    Put32(h, kSampleRate);
    Put32(h, kSampleRate * 2);   // byte rate
    Put16(h, 2);                 // block align
    Put16(h, 16);                // bits per sample
    h.insert(h.end(), {'d', 'a', 't', 'a'});
    Put32(h, data_bytes);
    return h;
}

std::vector<Recording> RecordingStore::List() const {
    std::vector<Recording> out;
    DIR* dir = opendir(dir_.c_str());
    if (!dir) return out;
    while (dirent* e = readdir(dir)) {
        const int n = ParseNumber(e->d_name);
        if (n < 0) continue;
        Recording r;
        r.number = n;
        r.path = dir_ + "/" + e->d_name;
        struct stat st;
        if (stat(r.path.c_str(), &st) == 0 && st.st_size > kWavHeaderBytes) {
            r.duration_ms = static_cast<int>((st.st_size - kWavHeaderBytes) / 2 * 1000 / kSampleRate);
        }
        out.push_back(std::move(r));
    }
    closedir(dir);
    std::sort(out.begin(), out.end(), [](const Recording& a, const Recording& b) { return a.number > b.number; });
    return out;
}

std::string RecordingStore::NextPath() const {
    const auto all = List();
    const int next = all.empty() ? 1 : all.front().number + 1;
    char name[32];
    snprintf(name, sizeof(name), "/rec_%04d.wav", next);
    return dir_ + name;
}

bool RecordingStore::Commit(std::string* final_path) {
    const std::string path = NextPath();
    if (rename(TempPath().c_str(), path.c_str()) != 0) return false;
    if (final_path) *final_path = path;
    const auto all = List();
    for (size_t i = kMaxRecordings; i < all.size(); ++i) remove(all[i].path.c_str());
    return true;
}

bool RecordingStore::Remove(const std::string& path) { return remove(path.c_str()) == 0; }

}  // namespace voice
