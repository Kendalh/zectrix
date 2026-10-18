#include "voice/esp_voice_backend.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/sleep_manager.h"

// Board LED (boards/zectrix-s3-epaper-4.2): blinks at 1 Hz while enabled with blink=true.
extern "C" void ZectrixSetFactoryLedOverride(bool enabled, bool blink);

namespace voice {

namespace {
constexpr char kTag[] = "VoiceBackend";
constexpr int kBufferSamples = kMaxRecordingMs * kSampleRate / 1000;
constexpr size_t kPlayChunkSamples = kSampleRate * 60 / 1000;  // one Opus frame worth, like AudioService
constexpr size_t kWriteChunkBytes = 4096;
constexpr int kStartBeepHz = 1200;
constexpr int kStopBeepHz = 700;
constexpr int kBeepMs = 150;
// The first output after a pause switches the amplifier / DAC on, which swallows the start of a sound.
// The start beep is therefore preceded by silence (the stop beep follows an enabled output, no need).
constexpr int kStartPrerollMs = 180;
constexpr int kBeepAmplitude = 6000;
// The start beep leaks into the microphone; drop this much of the beginning of the capture.
constexpr int kSkipAfterStartSamples = kSampleRate * 450 / 1000;
}  // namespace

// Short tone through the playback queue, optionally preceded by `preroll_ms` of silence. Blocks only while
// the (two frame) queue is full, i.e. about as long as the sound itself.
void EspVoiceBackend::Beep(int hz, int preroll_ms) {
    const int total = kSampleRate * kBeepMs / 1000;
    const int fade = kSampleRate * 5 / 1000;
    audio_->UnmuteOutput();
    for (int left = kSampleRate * preroll_ms / 1000; left > 0; left -= kPlayChunkSamples) {
        audio_->PushPcmToPlaybackQueue(std::vector<int16_t>(std::min<size_t>(left, kPlayChunkSamples), 0));
    }
    std::vector<int16_t> frame;
    for (int i = 0; i < total; ++i) {
        float env = 1.0f;
        if (i < fade) env = static_cast<float>(i) / fade;
        else if (i > total - fade) env = static_cast<float>(total - i) / fade;
        frame.push_back(static_cast<int16_t>(kBeepAmplitude * env * sinf(2.0f * 3.14159265f * hz * i / kSampleRate)));
        if (frame.size() == kPlayChunkSamples || i == total - 1) {
            audio_->PushPcmToPlaybackQueue(std::move(frame));
            frame.clear();
        }
    }
}

EspVoiceBackend::EspVoiceBackend(AudioService* audio, RecordingStore* store) : audio_(audio), store_(store) {}

EspVoiceBackend::~EspVoiceBackend() {
    StopPlayback();
    if (state_ == BackendState::Recording) StopRecording();
}

std::string EspVoiceBackend::LastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
}

void EspVoiceBackend::SetError(const char* msg) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    error_ = msg;
}

void EspVoiceBackend::SetState(BackendState state) {
    state_.store(state);
}

bool EspVoiceBackend::StartRecording() {
    if (state_ != BackendState::Idle || task_running_) return false;
    SetError("");

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (!buffer_) {
            buffer_ = static_cast<int16_t*>(
                heap_caps_malloc(kBufferSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if (!buffer_) {
            SetError("内存不足");
            ESP_LOGE(kTag, "No PSRAM for the recording buffer");
            return false;
        }
        recorded_samples_ = 0;
        skip_samples_ = kSkipAfterStartSamples;
    }

    SleepManager::GetInstance().Hold("voice");
    audio_->SetPcmSink([this](const std::vector<int16_t>& pcm) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (!buffer_) return;
        if (skip_samples_ > 0) {  // beep leakage + codec warm-up
            skip_samples_ -= static_cast<int>(pcm.size());
            return;
        }
        const int have = recorded_samples_.load();
        const int n = std::min<int>(static_cast<int>(pcm.size()), kBufferSamples - have);
        if (n <= 0) return;
        memcpy(buffer_ + have, pcm.data(), n * sizeof(int16_t));
        recorded_samples_ = have + n;
    });
    SetState(BackendState::Recording);
    audio_->EnableVoiceProcessing(true);  // clears the playback queue: beep only afterwards
    ZectrixSetFactoryLedOverride(true, true);
    Beep(kStartBeepHz, kStartPrerollMs);
    ESP_LOGI(kTag, "Recording started");
    return true;
}

void EspVoiceBackend::StopRecording() {
    if (state_ != BackendState::Recording) return;
    audio_->EnableVoiceProcessing(false);
    audio_->SetPcmSink(nullptr);  // returns only after a running sink call has finished
    ZectrixSetFactoryLedOverride(false, false);
    SetState(BackendState::Saving);
    task_running_ = true;
    if (xTaskCreate([](void* arg) {
            static_cast<EspVoiceBackend*>(arg)->SaveTask();
            vTaskDelete(nullptr);
        }, "voice_save", 4096, this, 3, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Failed to start the save task");
        task_running_ = false;
        SetError("保存失败");
        SetState(BackendState::Idle);
        SleepManager::GetInstance().Release("voice");
    }
}

void EspVoiceBackend::SaveTask() {
    const int samples = recorded_samples_.load();
    const int duration_ms = samples * 1000 / kSampleRate;
    bool changed = true;
    bool claimed = false;
    if (duration_ms < kMinRecordingMs) {
        changed = false;  // nothing new to show: skip the slow panel refresh
        ESP_LOGI(kTag, "Recording too short (%d ms), discarded", duration_ms);
    } else {
        const std::string tmp = store_->TempPath();
        FILE* f = fopen(tmp.c_str(), "wb");
        bool ok = f != nullptr;
        if (ok) {
            const uint32_t bytes = static_cast<uint32_t>(samples) * sizeof(int16_t);
            const auto header = MakeWavHeader(bytes);
            ok = fwrite(header.data(), 1, header.size(), f) == header.size();
            const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer_);
            for (uint32_t off = 0; ok && off < bytes; off += kWriteChunkBytes) {
                const size_t n = std::min<size_t>(kWriteChunkBytes, bytes - off);
                ok = fwrite(data + off, 1, n, f) == n;
            }
            ok = (fclose(f) == 0) && ok;
        }
        std::string final_path;
        if (ok && store_->Commit(&final_path)) {
            ESP_LOGI(kTag, "Saved %s (%d ms)", final_path.c_str(), duration_ms);
            Beep(kStopBeepHz, 0);
            claimed = NotifySaved(final_path);  // the AI app takes (and deletes) its own recording
        } else {
            ESP_LOGE(kTag, "Failed to save the recording");
            remove(tmp.c_str());
            SetError("保存失败(空间不足?)");
        }
    }
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        heap_caps_free(buffer_);
        buffer_ = nullptr;
        recorded_samples_ = 0;
    }
    SleepManager::GetInstance().Release("voice");
    SetState(BackendState::Idle);
    task_running_ = false;
    if (changed && !claimed) NotifyChanged();
}

bool EspVoiceBackend::StartPlayback(const std::string& path) {
    if (state_ != BackendState::Idle || task_running_) return false;
    SetError("");
    play_path_ = path;
    play_cancel_ = false;
    task_running_ = true;
    SleepManager::GetInstance().Hold("voice");
    SetState(BackendState::Playing);
    if (xTaskCreate([](void* arg) {
            static_cast<EspVoiceBackend*>(arg)->PlayTask();
            vTaskDelete(nullptr);
        }, "voice_play", 6144, this, 3, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Failed to start the play task");
        task_running_ = false;
        SetError("播放失败");
        SetState(BackendState::Idle);
        SleepManager::GetInstance().Release("voice");
        return false;
    }
    return true;
}

void EspVoiceBackend::PlayTask() {
    FILE* f = fopen(play_path_.c_str(), "rb");
    if (!f) {
        SetError("无法打开录音");
    } else {
        fseek(f, kWavHeaderBytes, SEEK_SET);
        audio_->UnmuteOutput();
        std::vector<int16_t> chunk(kPlayChunkSamples);
        while (!play_cancel_) {
            const size_t n = fread(chunk.data(), sizeof(int16_t), kPlayChunkSamples, f);
            if (n == 0) break;
            std::vector<int16_t> frame(chunk.begin(), chunk.begin() + n);
            if (!audio_->PushPcmToPlaybackQueue(std::move(frame))) break;
        }
        fclose(f);
        // Let the queued tail finish (the output task writes it to the codec).
        for (int i = 0; i < 40 && !play_cancel_ && !audio_->IsIdle(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    const bool failed = !LastError().empty();
    SleepManager::GetInstance().Release("voice");
    SetState(BackendState::Idle);
    task_running_ = false;
    if (failed) NotifyChanged();  // only a failure is worth a (slow) redraw: it shows the error
}

void EspVoiceBackend::StopPlayback() {
    if (state_ != BackendState::Playing) return;
    play_cancel_ = true;
    audio_->ResetDecoder();  // drop what is still queued
    for (int i = 0; i < 100 && task_running_; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace voice
