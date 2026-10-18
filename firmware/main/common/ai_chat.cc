#include "ai_chat.h"

#include <cJSON.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>

namespace ai {

namespace {

const char kSystemPrompt[] =
    "你是墨水屏设备上的 AI 助手。屏幕很小且刷新很慢：回答要简洁直接，使用纯文本，"
    "不要使用 Markdown 格式，一般不超过 300 字。";

bool IsUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

}  // namespace

std::vector<Message> TrimHistory(const std::vector<Message>& history) {
    size_t start = history.size();
    size_t chars = 0;
    while (start > 0) {
        const size_t next = chars + history[start - 1].text.size();
        if (next > kMaxContextChars && start < history.size()) break;  // always keep the newest message
        chars = next;
        --start;
    }
    while (start < history.size() && history[start].role != Message::User) ++start;  // begin with a question
    return std::vector<Message>(history.begin() + start, history.end());
}

std::string BuildRequestJson(const ChatConfig& config, const std::vector<Message>& history) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", config.model.c_str());
    cJSON_AddBoolToObject(root, "stream", false);  // the whole answer at once: the panel is too slow to stream
    cJSON_AddNumberToObject(root, "max_tokens", config.max_tokens);
    if (config.web_search) {  // without forced_search the model usually answers "I cannot get real-time information"
        cJSON_AddBoolToObject(root, "enable_search", true);
        cJSON* options = cJSON_AddObjectToObject(root, "search_options");
        cJSON_AddBoolToObject(options, "forced_search", true);
    }
    cJSON* messages = cJSON_AddArrayToObject(root, "messages");
    auto add = [messages](const char* role, const std::string& text) {
        cJSON* m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", role);
        cJSON_AddStringToObject(m, "content", text.c_str());
        cJSON_AddItemToArray(messages, m);
    };
    add("system", kSystemPrompt);
    for (const Message& m : TrimHistory(history)) add(m.role == Message::User ? "user" : "assistant", m.text);
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

bool ParseResponse(const std::string& body, std::string* content, std::string* error) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        *error = "响应格式错误";
        return false;
    }
    bool ok = false;
    cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON* message = cJSON_GetObjectItemCaseSensitive(err, "message");
    cJSON* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON* first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
    cJSON* answer = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(first, "message"), "content");
    if (cJSON_IsString(answer) && answer->valuestring && answer->valuestring[0]) {
        *content = answer->valuestring;
        ok = true;
    } else if (cJSON_IsString(message) && message->valuestring) {
        *error = message->valuestring;
    } else {
        *error = "没有收到回答";
    }
    cJSON_Delete(root);
    return ok;
}

std::string ToPlainText(const std::string& markdown) {
    std::string out;
    size_t pos = 0;
    while (pos <= markdown.size()) {
        size_t eol = markdown.find('\n', pos);
        if (eol == std::string::npos) eol = markdown.size();
        std::string line = markdown.substr(pos, eol - pos);
        pos = eol + 1;
        const std::string t = Trim(line);
        if (t.rfind("```", 0) == 0) continue;  // code fence marker
        size_t hashes = 0;
        while (hashes < t.size() && t[hashes] == '#') ++hashes;
        if (hashes > 0 && hashes < t.size() && t[hashes] == ' ') line = t.substr(hashes + 1);
        for (size_t at; (at = line.find("**")) != std::string::npos;) line.erase(at, 2);
        out += line;
        if (eol < markdown.size()) out += '\n';
        if (eol >= markdown.size()) break;
    }
    return Trim(out);
}

void SpeechBodyParts(const ChatConfig& config, std::string* prefix, std::string* suffix) {
    *prefix = "{\"model\":\"" + config.speech_model +
              "\",\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\","
              "\"input_audio\":{\"data\":\"data:audio/wav;base64,";
    *suffix = "\"}}]}],\"asr_options\":{\"language\":\"" + config.speech_language + "\",\"enable_itn\":false}}";
}

bool IsUsableTranscript(const std::string& text) {
    static const char* const kIgnored[] = {"。", "，", "！", "？", "、", "…", "；", "：", "“", "”", "～", "."};
    size_t letters = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t n = 1;
        if (c >= 0xF0) n = 4;
        else if (c >= 0xE0) n = 3;
        else if (c >= 0xC0) n = 2;
        const std::string ch = text.substr(i, n);
        i += n;
        bool ignored = n == 1 && !isalnum(c);
        for (const char* p : kIgnored) ignored = ignored || ch == p;
        if (!ignored) ++letters;
    }
    return letters >= 2;  // silence and breaths come back as "嗯。"
}

std::string Base64Encode(const std::string& data) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8) |
                           static_cast<uint8_t>(data[i + 2]);
        out += kTable[v >> 18];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += kTable[v & 63];
    }
    if (i + 1 == data.size()) {
        const uint32_t v = static_cast<uint8_t>(data[i]) << 16;
        out += kTable[v >> 18];
        out += kTable[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8);
        out += kTable[v >> 18];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool BuildFileBody(ChatPlatform::HttpRequest* request) {
    FILE* f = fopen(request->body_file.c_str(), "rb");
    std::string wav;
    if (f) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) wav.append(buf, n);
        fclose(f);
    }
    remove(request->body_file.c_str());  // recorded only for this question
    if (wav.empty()) return false;
    request->body = request->body_prefix + Base64Encode(wav) + request->body_suffix;
    return true;
}

std::string MakeTitle(const std::string& text, size_t max_chars) {
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (!IsUtf8Continuation(c)) {
            if (chars == max_chars) return out + "…";
            ++chars;
        }
        out += (c == '\n' || c == '\r') ? ' ' : static_cast<char>(c);
    }
    return out;
}

// ---- ChatService ------------------------------------------------------------------------------

ChatService& ChatService::Get() {
    static ChatService instance;
    return instance;
}

void ChatService::Start(std::unique_ptr<ChatPlatform> platform) {
    std::lock_guard<std::mutex> lock(mu_);
    if (platform_) return;
    platform_ = std::move(platform);
    api_key_ = Trim(platform_->LoadApiKey());
    if (sessions_.empty()) NewSessionLocked();
}

bool ChatService::Started() {
    std::lock_guard<std::mutex> lock(mu_);
    return platform_ != nullptr;
}

void ChatService::SetOnChange(std::function<void()> on_change) {
    std::lock_guard<std::mutex> lock(mu_);
    on_change_ = std::move(on_change);
}

void ChatService::Notify() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = on_change_;
    }
    if (cb) cb();
}

bool ChatService::HasApiKey() {
    std::lock_guard<std::mutex> lock(mu_);
    return !api_key_.empty();
}

namespace {
bool IsValidKey(const std::string& k) {
    if (k.empty() || k.size() > 200) return false;
    for (char c : k) {
        if (isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\\' || iscntrl(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}
}  // namespace

bool ChatService::SetApiKey(const std::string& key) {
    const std::string k = Trim(key);
    if (!IsValidKey(k)) return false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!platform_) return false;
        api_key_ = k;
        platform_->SaveApiKey(k);
    }
    Notify();
    return true;
}

Session* ChatService::Find(int id) {
    for (Session& s : sessions_)
        if (s.id == id) return &s;
    return nullptr;
}

int ChatService::NewSessionLocked() {
    if (sessions_.size() >= kMaxSessions) return -1;
    Session s;
    s.id = next_id_++;
    sessions_.push_back(std::move(s));
    current_ = sessions_.back().id;
    return current_;
}

int ChatService::CreateSession() {
    int id;
    {
        std::lock_guard<std::mutex> lock(mu_);
        id = NewSessionLocked();
    }
    if (id >= 0) Notify();
    return id;
}

bool ChatService::DeleteSession(int id) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = std::find_if(sessions_.begin(), sessions_.end(), [id](const Session& s) { return s.id == id; });
        if (it == sessions_.end()) return false;
        const size_t index = static_cast<size_t>(it - sessions_.begin());
        sessions_.erase(it);  // a reply still in flight for it is dropped by OnPostDone
        if (sessions_.empty()) {
            NewSessionLocked();
        } else if (current_ == id) {
            current_ = sessions_[std::min(index, sessions_.size() - 1)].id;
        }
    }
    Notify();
    return true;
}

bool ChatService::SelectSession(int id) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!Find(id)) return false;
        current_ = id;
    }
    Notify();
    return true;
}

int ChatService::CurrentSessionId() {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
}

int ChatService::CycleSession(int direction) {
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (sessions_.empty()) return 0;
        size_t index = 0;
        for (size_t i = 0; i < sessions_.size(); ++i)
            if (sessions_[i].id == current_) index = i;
        const int n = static_cast<int>(sessions_.size());
        index = static_cast<size_t>((static_cast<int>(index) + direction % n + n) % n);
        current_ = id = sessions_[index].id;
    }
    Notify();
    return id;
}

bool ChatService::BeginLocked(int id, bool voice, const std::string& question, std::string* error) {
    Session* s = Find(id);
    if (!platform_) *error = "AI 服务未启动（设备需要联网）";
    else if (api_key_.empty()) *error = "请先设置百炼 API Key";
    else if (!s) *error = "会话不存在";
    else if (!voice && question.empty()) *error = "问题不能为空";
    else if (question.size() > kMaxQuestionBytes) *error = "问题太长";
    else if (busy_) *error = "AI 正在回答另一个问题，请稍候";
    else error->clear();
    return error->empty();
}

bool ChatService::VoiceReady(int id, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    return BeginLocked(id, true, "", error);
}

void ChatService::SetSessionError(int id, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        Session* s = Find(id);
        if (!s || s->pending) return;
        s->last_error = message;
    }
    Notify();
}

bool ChatService::Ask(int id, const std::string& text, std::string* error) {
    const std::string question = Trim(text);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!BeginLocked(id, false, question, error)) return false;
        Find(id)->pending = true;
        Find(id)->last_error.clear();
        busy_ = true;
    }
    StartChat(id, question);
    return true;
}

bool ChatService::AskVoice(int id, const std::string& wav_path, std::string* error) {
    ChatPlatform* platform = nullptr;
    ChatConfig config;
    std::string key;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!BeginLocked(id, true, "", error)) return false;
        Session* s = Find(id);
        s->pending = true;
        s->transcribing = true;
        s->last_error.clear();
        busy_ = true;
        platform = platform_.get();
        config = config_;
        key = api_key_;
    }
    Notify();
    ChatPlatform::HttpRequest request;
    request.url = config.url;
    request.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + key}};
    request.body_file = wav_path;  // the platform reads and deletes it on its own task
    SpeechBodyParts(config, &request.body_prefix, &request.body_suffix);
    platform->Send(std::move(request), [this, id](int status, std::string body, std::string transport_error) {
        OnTranscribed(id, status, std::move(body), std::move(transport_error));
    });
    return true;
}

void ChatService::FailVoice(int id, const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        busy_ = false;
        if (Session* s = Find(id)) {
            s->pending = false;
            s->transcribing = false;
            s->last_error = "语音识别失败: " + error;
        }
    }
    Notify();
}

void ChatService::OnTranscribed(int id, int status, std::string body, std::string transport_error) {
    std::string text, error;
    bool ok = false;
    if (!transport_error.empty()) {
        error = "网络错误: " + transport_error;
    } else {
        ok = ParseResponse(body, &text, &error);  // the transcript is the assistant message's content
        if (!ok && status == 401) error = "API Key 无效 (401)";
        else if (!ok && status != 200 && (error == "没有收到回答" || error == "响应格式错误"))
            error = "请求失败 (HTTP " + std::to_string(status) + ")";
    }
    text = Trim(text);
    if (ok && !IsUsableTranscript(text)) {
        ok = false;
        error = "没听清，请再说一次";
    }
    if (ok && text.size() > kMaxQuestionBytes) {
        size_t cut = kMaxQuestionBytes;
        while (cut > 0 && IsUtf8Continuation(static_cast<unsigned char>(text[cut]))) --cut;
        text.resize(cut);
    }
    if (!ok) {
        FailVoice(id, error);
        return;
    }
    StartChat(id, text);
}

// Adds the question to the session and sends it to the chat model. busy_ is already set by the caller.
void ChatService::StartChat(int id, std::string question) {
    std::string request, url, key;
    ChatPlatform* platform = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        Session* s = Find(id);
        if (!s) {  // deleted while the recording was being transcribed
            busy_ = false;
            platform = nullptr;
        } else {
            if (s->messages.empty()) s->title = MakeTitle(question);
            s->messages.push_back({Message::User, question});
            s->pending = true;
            s->transcribing = false;
            request = BuildRequestJson(config_, s->messages);
            url = config_.url;
            key = api_key_;
            platform = platform_.get();
        }
    }
    Notify();  // the screen shows "thinking" while the answer is awaited
    if (!platform) return;
    ChatPlatform::HttpRequest http;
    http.url = url;
    http.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + key}};
    http.body = std::move(request);
    platform->Send(std::move(http), [this, id](int status, std::string body, std::string transport_error) {
        OnPostDone(id, status, std::move(body), std::move(transport_error));
    });
}

void ChatService::OnPostDone(int id, int status, std::string body, std::string transport_error) {
    std::string content, error;
    bool ok = false;
    if (!transport_error.empty()) {
        error = "网络错误: " + transport_error;
    } else {
        ok = ParseResponse(body, &content, &error);
        const bool generic = error == "没有收到回答" || error == "响应格式错误";  // e.g. a proxy's HTML error page
        if (!ok && status != 200 && generic) error = "请求失败 (HTTP " + std::to_string(status) + ")";
        if (!ok && status == 401) error = "API Key 无效 (401)";
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        busy_ = false;
        if (Session* s = Find(id)) {  // may have been deleted while waiting
            s->pending = false;
            s->transcribing = false;
            if (ok) {
                s->messages.push_back({Message::Assistant, ToPlainText(content)});
                while (s->messages.size() > kMaxStoredMessages) s->messages.erase(s->messages.begin());
                while (!s->messages.empty() && s->messages.front().role != Message::User)
                    s->messages.erase(s->messages.begin());
            } else {
                // Forget the unanswered question so the context stays a clean alternation; the UI shows why.
                if (!s->messages.empty() && s->messages.back().role == Message::User) s->messages.pop_back();
                if (s->messages.empty()) s->title.clear();
                s->last_error = error;
            }
        }
    }
    Notify();
}

std::vector<SessionInfo> ChatService::List() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SessionInfo> out;
    for (const Session& s : sessions_) out.push_back({s.id, s.title, s.messages.size(), s.pending, s.transcribing});
    return out;
}

bool ChatService::Snapshot(int id, Session* out) {
    std::lock_guard<std::mutex> lock(mu_);
    Session* s = Find(id);
    if (!s) return false;
    *out = *s;
    return true;
}

}  // namespace ai
