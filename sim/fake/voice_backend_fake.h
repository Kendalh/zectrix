// Simulator stand-in for the firmware VoiceBackend (voice/esp_voice_backend.cc): no audio hardware.
// "Recording" writes a silent WAV whose length is the number of seconds set by set_fake_record_ms().
#pragma once

#include "common/voice_backend.h"
#include "common/voice_recordings.h"

namespace voice {

class FakeVoiceBackend : public VoiceBackend {
public:
    explicit FakeVoiceBackend(RecordingStore* store) : store_(store) {}

    void set_fake_record_ms(int ms) { fake_record_ms_ = ms; }

    bool StartRecording() override;
    void StopRecording() override;
    bool StartPlayback(const std::string& path) override;
    void StopPlayback() override;
    BackendState State() const override { return state_; }
    int RecordedMs() const override { return state_ == BackendState::Recording ? fake_record_ms_ : 0; }
    std::string LastError() const override { return ""; }

private:
    RecordingStore* store_;
    BackendState state_ = BackendState::Idle;
    int fake_record_ms_ = 2000;
};

}  // namespace voice
