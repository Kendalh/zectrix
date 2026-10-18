/**
 * @file esp_voice_backend.h
 * @brief Firmware VoiceBackend: records to PSRAM through AudioService's PCM tap, saves WAV files in a
 *        background task and plays them back through AudioService's playback queue.
 */

#ifndef VOICE_ESP_VOICE_BACKEND_H
#define VOICE_ESP_VOICE_BACKEND_H

#include <atomic>
#include <mutex>

#include "audio/audio_service.h"
#include "common/voice_backend.h"
#include "common/voice_recordings.h"

namespace voice {

class EspVoiceBackend : public VoiceBackend {
public:
    EspVoiceBackend(AudioService* audio, RecordingStore* store);
    ~EspVoiceBackend() override;

    bool StartRecording() override;
    void StopRecording() override;
    bool StartPlayback(const std::string& path) override;
    void StopPlayback() override;

    BackendState State() const override { return state_.load(); }
    int RecordedMs() const override { return recorded_samples_.load() * 1000 / kSampleRate; }
    std::string LastError() const override;

private:
    void SaveTask();
    void PlayTask();
    void SetError(const char* msg);
    void SetState(BackendState state);
    void Beep(int hz, int preroll_ms);

    AudioService* audio_;
    RecordingStore* store_;
    std::atomic<BackendState> state_{BackendState::Idle};

    std::mutex buffer_mutex_;     // guards buffer_ / capacity writes between the sink and the save task
    int16_t* buffer_ = nullptr;   // PSRAM, kMaxRecordingMs worth of samples
    std::atomic<int> recorded_samples_{0};
    int skip_samples_ = 0;        // guarded by buffer_mutex_

    std::string play_path_;
    std::atomic<bool> play_cancel_{false};
    std::atomic<bool> task_running_{false};

    mutable std::mutex error_mutex_;
    std::string error_;
};

}  // namespace voice

#endif  // VOICE_ESP_VOICE_BACKEND_H
