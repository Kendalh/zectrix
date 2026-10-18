/**
 * @file ai_chat.h
 * @brief AI Q&A (Alibaba Bailian Qwen, non-streaming): sessions with remembered context.
 *
 * The e-ink panel refreshes too slowly for streamed text, so a question is answered with one
 * non-streaming request and the whole answer is shown at once. Each session keeps its own message
 * history, which is sent with every question (trimmed to kMaxContextChars) so the model remembers
 * the conversation; several sessions can exist side by side. Sessions live in RAM only.
 *
 * Everything here is hardware-free and shared with the simulator. The network and the API key
 * storage sit behind ChatPlatform (firmware: common/ai_chat_platform_esp.cc, sim: fake/ai_chat_platform_fake.cc).
 * Questions are spoken: the Voice recorder captures a WAV, the speech model (Qwen3-ASR) turns it into the
 * question text, and from there it is an ordinary question. Both models are Alibaba Bailian (DashScope) models
 * behind its OpenAI-compatible endpoint, so a single API key serves chat and speech recognition.
 * The API key is never compiled in: it is entered on the device web page (/ai) and stored in NVS.
 */

#ifndef AI_CHAT_H
#define AI_CHAT_H

#include <stddef.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ai {

constexpr size_t kMaxSessions = 8;
constexpr size_t kMaxStoredMessages = 40;    // per session; the oldest are dropped
constexpr size_t kMaxContextChars = 6000;    // history bytes sent with a question (newest kept)
constexpr size_t kMaxQuestionBytes = 1500;

struct ChatConfig {
    // Bailian (DashScope) OpenAI-compatible endpoint; the same URL serves chat and speech recognition.
    std::string url = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    std::string model = "qwen-flash";           // fast and cheap
    bool web_search = true;                     // Bailian built-in web search (enable_search), forced on every question
    int max_tokens = 800;                       // the panel only fits a few hundred characters anyway
    std::string speech_model = "qwen3-asr-flash";
    std::string speech_language = "zh";
};

struct Message {
    enum Role { User, Assistant };
    Role role = User;
    std::string text;
};

struct Session {
    int id = 0;
    std::string title;             // first question, shortened
    std::vector<Message> messages;
    bool pending = false;          // a question is being transcribed or answered
    bool transcribing = false;     // pending, and still turning the recording into text
    std::string last_error;        // why the last question failed; cleared by the next question
};

struct SessionInfo {
    int id = 0;
    std::string title;
    size_t message_count = 0;
    bool pending = false;
    bool transcribing = false;
};

// ---- pure helpers (unit-tested) ---------------------------------------------------------------

/** The newest messages that fit kMaxContextChars (always at least the last one), starting with a user turn. */
std::vector<Message> TrimHistory(const std::vector<Message>& history);

/** JSON body for POST /chat/completions: system prompt + trimmed history, "stream": false. */
std::string BuildRequestJson(const ChatConfig& config, const std::vector<Message>& history);

/** Extract the answer from a response body, or the API's error message. @return true on success. */
bool ParseResponse(const std::string& body, std::string* content, std::string* error);

/** Drop Markdown decoration the panel cannot render (**bold**, # headings, ``` fences). */
std::string ToPlainText(const std::string& markdown);

/**
 * Request body for recognising the WAV in `wav_base64`: a chat completion with an `input_audio` part, which is how
 * Qwen3-ASR takes audio on the OpenAI-compatible endpoint. The answer is the transcript (see ParseResponse).
 */
void SpeechBodyParts(const ChatConfig& config, std::string* prefix, std::string* suffix);

/** True when a transcript is worth asking: at least two letters/characters once punctuation is ignored. */
bool IsUsableTranscript(const std::string& text);

/** Standard base64 (with padding). */
std::string Base64Encode(const std::string& data);

/** First characters of text for a session title (UTF-8 safe). */
std::string MakeTitle(const std::string& text, size_t max_chars = 12);

// ---- platform ---------------------------------------------------------------------------------

class ChatPlatform {
public:
    virtual ~ChatPlatform() = default;

    /** transport_error is empty when an HTTP response arrived (status/body valid). May be called on any thread. */
    using DoneCallback = std::function<void(int status, std::string body, std::string transport_error)>;

    struct HttpRequest {
        std::string method = "POST";
        std::string url;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        /**
         * If set, the body is `body_prefix` + base64(content of this file) + `body_suffix`. It is assembled by the
         * platform on its own task (the caller may be a small-stack task) with BuildFileBody(), and the file is
         * deleted when the request is finished.
         */
        std::string body_file, body_prefix, body_suffix;
    };

    /** Start an HTTPS request and return immediately; `done` is called exactly once, off the caller's thread. */
    virtual void Send(HttpRequest request, DoneCallback done) = 0;
    virtual std::string LoadApiKey() = 0;
    virtual void SaveApiKey(const std::string& key) = 0;
};

/** Assemble request->body from body_file (see HttpRequest::body_file) and delete the file. @return false if unreadable. */
bool BuildFileBody(ChatPlatform::HttpRequest* request);

// ---- service ----------------------------------------------------------------------------------

class ChatService {
public:
    static ChatService& Get();

    /** Idempotent. Loads the API key and creates the first session. */
    void Start(std::unique_ptr<ChatPlatform> platform);
    bool Started();

    /** Called (on any thread, without internal locks held) whenever anything visible changed. */
    void SetOnChange(std::function<void()> on_change);

    bool HasApiKey();
    bool SetApiKey(const std::string& key);  // trimmed; empty or whitespace-containing keys are rejected

    /** Can a spoken question be started now? Otherwise *error says why (Chinese, user-facing). */
    bool VoiceReady(int id, std::string* error);
    /** Show `message` as the session's error (e.g. nothing recorded). */
    void SetSessionError(int id, const std::string& message);
    /**
     * Transcribe the recording at `wav_path` (deleted afterwards) and ask the text in session
     * `id`. Same rules and result as Ask.
     */
    bool AskVoice(int id, const std::string& wav_path, std::string* error);

    /** @return the new session id (it becomes the current one), or -1 when kMaxSessions exist. */
    int CreateSession();
    bool DeleteSession(int id);              // never leaves zero sessions
    bool SelectSession(int id);
    int CurrentSessionId();
    /** Switch to the next/previous session (wraps). Returns the new current id. */
    int CycleSession(int direction);

    /** Send a question in session `id`. @return false with *error (Chinese, user-facing) when not accepted. */
    bool Ask(int id, const std::string& text, std::string* error);

    std::vector<SessionInfo> List();
    bool Snapshot(int id, Session* out);

private:
    Session* Find(int id);
    bool BeginLocked(int id, bool voice, const std::string& question, std::string* error);
    void StartChat(int id, std::string text);   // called with busy_ already set
    void OnTranscribed(int id, int status, std::string body, std::string transport_error);
    void FailVoice(int id, const std::string& error);
    int NewSessionLocked();
    void OnPostDone(int id, int status, std::string body, std::string transport_error);
    void Notify();

    std::mutex mu_;
    std::unique_ptr<ChatPlatform> platform_;
    std::function<void()> on_change_;
    ChatConfig config_;
    std::string api_key_;
    std::vector<Session> sessions_;
    int current_ = 0;
    int next_id_ = 1;
    bool busy_ = false;  // one request in flight at a time (TLS buffers are large)

public:
    ChatService() = default;  // public for tests; the app uses Get()
};

}  // namespace ai

#endif  // AI_CHAT_H
