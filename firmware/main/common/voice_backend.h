/**
 * @file voice_backend.h
 * @brief Hardware side of the Voice app (microphone capture and speaker playback).
 *
 * The Voice page renderer only talks to this interface, so the PC simulator can supply a fake.
 * The firmware implementation is voice/esp_voice_backend.cc (on top of AudioService).
 * Recordings are 16 kHz mono 16-bit WAV files managed by common/voice_recordings.h.
 */

#ifndef COMMON_VOICE_BACKEND_H
#define COMMON_VOICE_BACKEND_H

#include <functional>
#include <string>

namespace voice {

enum class BackendState {
    Idle,
    Recording,
    Saving,    ///< Recording stopped, the WAV file is being written.
    Playing,
};

class VoiceBackend {
public:
    virtual ~VoiceBackend() = default;

    /** Start capturing from the microphone. Fails (false) unless Idle. */
    virtual bool StartRecording() = 0;
    /** Stop capturing; the file is saved asynchronously (state Saving, then Idle). */
    virtual void StopRecording() = 0;
    /** Play a WAV file. Fails (false) unless Idle. Returns to Idle when it ends. */
    virtual bool StartPlayback(const std::string& path) = 0;
    virtual void StopPlayback() = 0;

    virtual BackendState State() const = 0;
    /** Milliseconds captured so far (valid while Recording). */
    virtual int RecordedMs() const = 0;
    /** Empty when the last operation succeeded. */
    virtual std::string LastError() const = 0;

    /** Called (from any thread) whenever State() changes without a UI action, e.g. playback ended. */
    void SetOnChanged(std::function<void()> cb) { on_changed_ = std::move(cb); }

protected:
    void NotifyChanged() {
        if (on_changed_) on_changed_();
    }

private:
    std::function<void()> on_changed_;
};

}  // namespace voice

#endif  // COMMON_VOICE_BACKEND_H
