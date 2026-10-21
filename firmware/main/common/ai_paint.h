/**
 * @file ai_paint.h
 * @brief AI painter: speak a subject, get a four-colour (black/white/yellow/red) 400x300 picture, redraw it or save it.
 *
 * Pipeline (one API key, Alibaba Bailian / DashScope): the recording is transcribed by the speech model (same request as
 * the AI Q&A page), a text model plans the scene from the subject (PlannerSystemPrompt: time of day, sun or moon, ...), the
 * plan goes into the shared poster style (ImagePrompt) and to the image model, the
 * returned PNG is downloaded and turned into the panel's 2 bpp BWRY format (PngToBwry: area-average down to 400x300,
 * Floyd-Steinberg dither onto the four inks). The result is held in RAM; "redraw" asks the model again with the same
 * subject, "save" writes it to the photo album (photo_storage) as a 2 bpp photo.
 *
 * Everything here is hardware-free and shared with the simulator. Network and key storage go through ai::ChatPlatform
 * (the same interface as AI Q&A); the album goes through photo_storage.h (fake in the sim).
 */

#ifndef AI_PAINT_H
#define AI_PAINT_H

#include <stdint.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ai_chat.h"

namespace ai {
namespace paint {

constexpr int kWidth = 400;
constexpr int kHeight = 300;
constexpr size_t kImageBytes = kWidth * kHeight / 4;  // 2 bits per pixel: 00 black, 01 white, 10 yellow, 11 red
constexpr size_t kMaxSubjectChars = 60;
constexpr size_t kMaxPngBytes = 4 * 1024 * 1024;

struct PaintConfig {
    std::string url = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";
    std::string model = "qwen-image-2.0-pro";  // synchronous; qwen-image-3.0-pro needs a workspace-specific endpoint
    std::string size = "640*480";              // 4:3; the API rejects anything under 512x512 pixels, so 400x300 is not possible
};

// ---- pure helpers (unit-tested) ---------------------------------------------------------------

/** Trim, drop filler punctuation at both ends and cut to kMaxSubjectChars characters (UTF-8 safe). */
std::string NormalizeSubject(const std::string& raw);

/**
 * The system prompt of the planning step: a text model turns the spoken subject into one English scene description, choosing
 * time of day, weather and the big celestial disc (red sun at sunset, moon at night, ...) from the user's words.
 */
const char* PlannerSystemPrompt();

/** Chat-completions body for the planning step: system prompt + the subject. */
std::string BuildPlanRequestJson(const ChatConfig& config, const std::string& subject);

/** Tidy the planner's answer (quotes, Markdown, whitespace, length). @return false when nothing usable is left. */
bool CleanPlan(const std::string& raw, std::string* description);

/** The image prompt for a planned scene: the description inside the shared poster style. */
std::string ImagePrompt(const std::string& description);

/** The image prompt without a planning step (planner failed): the subject in a neutral version of the same style. */
std::string FallbackPrompt(const std::string& subject);
const char* NegativePrompt();

/** JSON body for the multimodal-generation endpoint. */
std::string BuildImageRequestJson(const PaintConfig& config, const std::string& prompt);

/** Image URL from a response body, or the API's error message. @return true on success. */
bool ParseImageResponse(const std::string& body, std::string* image_url, std::string* error);

/** The API's error text as a short Chinese line for the panel (rate limit, quota, ...); unknown texts pass through. */
std::string FriendlyError(const std::string& api_error);

/** PNG bytes -> kImageBytes of BWRY 2 bpp. @return false with *error (Chinese, user-facing). */
bool PngToBwry(const std::string& png, std::vector<uint8_t>* out, std::string* error);

// ---- service ----------------------------------------------------------------------------------

enum class Phase { Idle, Transcribing, Planning, Generating, Downloading, Converting, Ready, Error };

struct Status {
    Phase phase = Phase::Idle;
    std::string subject;
    std::string error;       // set in Phase::Error; the previous picture (if any) is kept
    bool has_image = false;
    bool saved = false;      // the current picture is in the album
    bool working = false;    // Transcribing .. Converting
    uint32_t revision = 0;   // increases with every new picture
};

class PaintService {
public:
    static PaintService& Get();

    /** Idempotent. Loads the API key. */
    void Start(std::unique_ptr<ChatPlatform> platform);
    bool Started();

    /** Called (on any thread, without internal locks held) whenever anything visible changed. */
    void SetOnChange(std::function<void()> on_change);

    bool HasApiKey();

    /** Can a spoken subject be accepted now? Otherwise *error says why (Chinese, user-facing). */
    bool VoiceReady(std::string* error);
    /** Show `message` as the error (e.g. nothing recorded). */
    void SetError(const std::string& message);
    /** Transcribe the recording at `wav_path` (deleted afterwards), then paint what was said. */
    bool AskVoice(const std::string& wav_path, std::string* error);
    /** Paint `subject` (already text). */
    bool Draw(const std::string& subject, std::string* error);
    /** Paint the current subject again. */
    bool Regenerate(std::string* error);
    /** Pause before a retry after a network error (ms); tests set 0. */
    void SetRetryDelayMs(int ms);
    /** Put the current picture into the album (once per picture). */
    bool SaveToAlbum(std::string* error);

    Status GetStatus();
    /** Copy of the current picture (kImageBytes), false when there is none. */
    bool CopyImage(std::vector<uint8_t>* out);

private:
    bool BeginLocked(Phase phase, std::string* error);
    void StartPlan(const std::string& subject);
    void OnPlanned(const std::string& subject, int status, std::string body, std::string transport_error);
    void StartGenerate(const std::string& subject, const std::string& prompt);
    void StartDownload(const std::string& subject, const std::string& url, bool plain_http);
    /** Send, and send again up to `retries` times when the request fails at the network level (no HTTP answer). */
    void SendWithRetry(ChatPlatform::HttpRequest request, int retries, ChatPlatform::DoneCallback done);
    void OnTranscribed(int status, std::string body, std::string transport_error);
    void OnGenerated(const std::string& subject, int status, std::string body, std::string transport_error);
    void OnDownloaded(const std::string& subject, int status, std::string body, std::string transport_error);
    void Fail(const std::string& error);
    void SetPhase(Phase phase);
    void Notify();

    std::mutex mu_;
    std::unique_ptr<ChatPlatform> platform_;
    std::function<void()> on_change_;
    PaintConfig paint_config_;
    ChatConfig chat_config_;  // speech model settings
    std::string api_key_;
    Status status_;
    std::vector<uint8_t> image_;
    bool busy_ = false;
    int retry_delay_ms_ = 1500;

public:
    PaintService() = default;  // public for tests; the app uses Get()
};

}  // namespace paint
}  // namespace ai

#endif  // AI_PAINT_H
