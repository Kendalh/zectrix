#include "fake/voice_backend_fake.h"

#include <cstdio>
#include <vector>

namespace voice {

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
    if (f) {
        const auto header = MakeWavHeader(bytes);
        fwrite(header.data(), 1, header.size(), f);
        std::vector<uint8_t> zeros(bytes, 0);
        fwrite(zeros.data(), 1, zeros.size(), f);
        fclose(f);
        store_->Commit(nullptr);
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
