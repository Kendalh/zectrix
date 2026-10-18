/**
 * @file voice_recordings.h
 * @brief Recording files of the Voice app: naming, listing, WAV header. Hardware free (plain stdio/dirent),
 *        shared by the firmware and the simulator.
 *
 * Files are `<dir>/rec_NNNN.wav` (flat: SPIFFS has no directories and a 32 character path limit).
 * While being written a file is called `<dir>/rec.tmp`, so a half written recording is never listed.
 */

#ifndef COMMON_VOICE_RECORDINGS_H
#define COMMON_VOICE_RECORDINGS_H

#include <cstdint>
#include <string>
#include <vector>

namespace voice {

constexpr int kSampleRate = 16000;
constexpr int kWavHeaderBytes = 44;
constexpr int kMaxRecordingMs = 30000;   ///< Capture stops by itself after this long.
constexpr int kMinRecordingMs = 300;     ///< Shorter captures (accidental press) are discarded.
constexpr int kMaxRecordings = 20;       ///< Oldest files are deleted beyond this.

struct Recording {
    std::string path;
    int number = 0;       ///< NNNN of the file name.
    int duration_ms = 0;  ///< Derived from the file size.
};

/** Fixed-size WAV header (PCM, mono, 16 bit, kSampleRate) for `data_bytes` bytes of samples. */
std::vector<uint8_t> MakeWavHeader(uint32_t data_bytes);

class RecordingStore {
public:
    explicit RecordingStore(std::string dir) : dir_(std::move(dir)) {}

    /** Newest first. */
    std::vector<Recording> List() const;
    std::string TempPath() const { return dir_ + "/rec.tmp"; }
    /** Path of the next recording (highest existing number + 1). */
    std::string NextPath() const;
    /** Rename the temp file to NextPath(), then delete the oldest files beyond kMaxRecordings. */
    bool Commit(std::string* final_path);
    bool Remove(const std::string& path);

private:
    std::string dir_;
};

}  // namespace voice

#endif  // COMMON_VOICE_RECORDINGS_H
