#include "fake/voice_backend_fake.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace voice {

namespace {
const char kMicFile[] = "/tmp/zectrix_sim_mic.wav";  // written by sim/web/server.py from the browser microphone
}

bool FakeVoiceBackend::StartRecording() {
    if (state_ != BackendState::Idle) return false;
    state_ = BackendState::Recording;
    return true;
}

void FakeVoiceBackend::StopRecording() {
    if (state_ != BackendState::Recording) return;
    // The firmware saves in a background task (Saving -> Idle); the sim finishes at once.
    const uint32_t bytes = static_cast<uint32_t>(fake_record_ms_) * kSampleRate / 1000 * 2;
    FILE* f = fopen(store_->TempPath().c_str(), "wb");
    // SIM_VOICE_WAV=<file.wav>: "record" that file instead of silence (to try real speech recognition).
    // The browser microphone (a file the web page uploaded just before releasing UP) wins; it is used once.
    std::string mic_path;
    if (FILE* probe = fopen(kMicFile, "rb")) {
        fclose(probe);
        mic_path = std::string(kMicFile) + ".take";
        rename(kMicFile, mic_path.c_str());
    }
    const char* real = mic_path.empty() ? getenv("SIM_VOICE_WAV") : mic_path.c_str();
    if (real) {
        if (FILE* in = fopen(real, "rb")) {
            char buf[4096];
            size_t n;
            while (f && (n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, f);
            fclose(in);
            if (!mic_path.empty()) remove(mic_path.c_str());
            if (f) {
                fclose(f);
                std::string path;
                if (store_->Commit(&path)) NotifySaved(path);
            }
            state_ = BackendState::Idle;
            return;
        }
    }
    if (f) {
        const auto header = MakeWavHeader(bytes);
        fwrite(header.data(), 1, header.size(), f);
        std::vector<uint8_t> zeros(bytes, 0);
        fwrite(zeros.data(), 1, zeros.size(), f);
        fclose(f);
        std::string path;
        if (store_->Commit(&path)) NotifySaved(path);
    }
    state_ = BackendState::Idle;
}

bool FakeVoiceBackend::StartPlayback(const std::string&) {
    if (state_ != BackendState::Idle) return false;
    state_ = BackendState::Playing;  // never ends by itself; BOOT stops it
    return true;
}

void FakeVoiceBackend::StopPlayback() {
    if (state_ == BackendState::Playing) state_ = BackendState::Idle;
}

}  // namespace voice
