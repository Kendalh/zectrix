// Unit tests for common/ai_chat.cc: request building, response parsing, context trimming and the
// session service (with a scripted platform instead of the network).
#include <cstdio>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include <cJSON.h>

#include "common/ai_chat.h"

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)

using ai::Message;

namespace {

// Captures the request; the test completes it by hand with Reply()/Fail().
class ScriptedPlatform : public ai::ChatPlatform {
public:
    void Send(HttpRequest r, DoneCallback d) override {
        url = r.url;
        done = std::move(d);
        for (const auto& h : r.headers)
            if (h.first == "Authorization") api_key = h.second.substr(7);  // after "Bearer "
        if (!r.body_file.empty()) {  // speech recognition of a recording
            wav_path = r.body_file;
            asr_prefix = r.body_prefix;
            asr_suffix = r.body_suffix;
            ++asr_requests;
        } else {
            request = r.body;
            ++posts;
        }
    }
    std::string LoadApiKey() override { return stored_key; }
    void SaveApiKey(const std::string& k) override { stored_key = k; }

    void Reply(const std::string& text) {
        cJSON* r = cJSON_CreateObject();
        cJSON* choices = cJSON_AddArrayToObject(r, "choices");
        cJSON* c = cJSON_CreateObject();
        cJSON* m = cJSON_AddObjectToObject(c, "message");
        cJSON_AddStringToObject(m, "content", text.c_str());
        cJSON_AddItemToArray(choices, c);
        char* s = cJSON_PrintUnformatted(r);
        auto cb = std::move(done);
        cb(200, s, "");
        cJSON_free(s);
        cJSON_Delete(r);
    }
    void Fail(int status, const std::string& body, const std::string& transport = "") {
        auto cb = std::move(done);
        cb(status, body, transport);
    }

    std::string url, api_key, request, stored_key = "sk-test";
    std::string wav_path, asr_prefix, asr_suffix;
    DoneCallback done;
    int posts = 0, asr_requests = 0;
};

struct Fixture {
    ScriptedPlatform* platform = new ScriptedPlatform;
    ai::ChatService service;
    int changes = 0;
    Fixture(const std::string& key = "sk-test") {
        platform->stored_key = key;
        service.SetOnChange([this]() { ++changes; });
        service.Start(std::unique_ptr<ai::ChatPlatform>(platform));
    }
};

std::vector<std::string> RequestRoles(const std::string& request, std::vector<std::string>* contents = nullptr) {
    std::vector<std::string> roles;
    cJSON* root = cJSON_Parse(request.c_str());
    cJSON* m = nullptr;
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(root, "messages")) {
        roles.push_back(cJSON_GetObjectItemCaseSensitive(m, "role")->valuestring);
        if (contents) contents->push_back(cJSON_GetObjectItemCaseSensitive(m, "content")->valuestring);
    }
    cJSON_Delete(root);
    return roles;
}

void TestRequestJson() {
    ai::ChatConfig config;
    const std::string json = ai::BuildRequestJson(config, {{Message::User, "你好 \"引号\"\n换行"}});
    cJSON* root = cJSON_Parse(json.c_str());
    CHECK(root != nullptr);
    CHECK(std::string(cJSON_GetObjectItemCaseSensitive(root, "model")->valuestring) == "qwen-flash");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "stream")));  // non-streaming
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "enable_search")));  // web search
    cJSON* options = cJSON_GetObjectItemCaseSensitive(root, "search_options");
    CHECK(options && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(options, "forced_search")));
    std::vector<std::string> contents;
    const auto roles = RequestRoles(json, &contents);
    CHECK(roles.size() == 2 && roles[0] == "system" && roles[1] == "user");
    CHECK(contents[1] == "你好 \"引号\"\n换行");  // escaping round-trips
    cJSON_Delete(root);
}

void TestParseResponse() {
    std::string content, error;
    CHECK(ai::ParseResponse(R"({"choices":[{"message":{"role":"assistant","content":"答案"}}]})", &content, &error));
    CHECK(content == "答案");
    CHECK(!ai::ParseResponse(R"({"error":{"message":"Authentication Fails","type":"auth"}})", &content, &error));
    CHECK(error == "Authentication Fails");
    CHECK(!ai::ParseResponse("<html>502</html>", &content, &error));
    CHECK(!ai::ParseResponse(R"({"choices":[]})", &content, &error));
    CHECK(!ai::ParseResponse(R"({"choices":[{"message":{"content":""}}]})", &content, &error));
}

void TestPlainTextAndTitle() {
    CHECK(ai::ToPlainText("## 标题\n这是 **重点** 内容\n```\ncode\n```") == "标题\n这是 重点 内容\ncode");
    CHECK(ai::ToPlainText("plain") == "plain");
    CHECK(ai::MakeTitle("短问题") == "短问题");
    CHECK(ai::MakeTitle("一二三四五六七八九十一二三四五") == "一二三四五六七八九十一二…");  // 12 chars, UTF-8 safe
    CHECK(ai::MakeTitle("a\nb") == "a b");
}

void TestTrimHistory() {
    std::vector<Message> h;
    for (int i = 0; i < 6; ++i) {
        h.push_back({Message::User, std::string(1500, 'q')});
        h.push_back({Message::Assistant, std::string(1500, 'a')});
    }
    const auto trimmed = ai::TrimHistory(h);
    size_t chars = 0;
    for (const auto& m : trimmed) chars += m.text.size();
    CHECK(chars <= ai::kMaxContextChars);
    CHECK(!trimmed.empty() && trimmed.front().role == Message::User);  // starts with a question
    CHECK(trimmed.back().role == Message::Assistant);                   // newest kept
    // A single oversized message is still sent.
    CHECK(ai::TrimHistory({{Message::User, std::string(9000, 'x')}}).size() == 1);
}

void TestSessionKeepsContext() {
    Fixture f;
    std::string error;
    const int s1 = f.service.CurrentSessionId();
    CHECK(f.service.Ask(s1, "我叫小明", &error));
    CHECK(f.platform->url == "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
    CHECK(f.platform->api_key == "sk-test");
    CHECK(f.service.List()[0].pending);
    f.platform->Reply("你好小明");
    CHECK(!f.service.List()[0].pending);

    CHECK(f.service.Ask(s1, "我叫什么？", &error));
    std::vector<std::string> contents;
    const auto roles = RequestRoles(f.platform->request, &contents);
    CHECK((roles == std::vector<std::string>{"system", "user", "assistant", "user"}));
    CHECK(contents[1] == "我叫小明" && contents[2] == "你好小明" && contents[3] == "我叫什么？");
    f.platform->Reply("你叫小明");

    ai::Session snap;
    CHECK(f.service.Snapshot(s1, &snap));
    CHECK(snap.messages.size() == 4 && snap.title == "我叫小明");
}

void TestSessionsAreIsolated() {
    Fixture f;
    std::string error;
    const int s1 = f.service.CurrentSessionId();
    CHECK(f.service.Ask(s1, "第一个会话的问题", &error));
    f.platform->Reply("答1");
    const int s2 = f.service.CreateSession();
    CHECK(s2 > 0 && s2 != s1 && f.service.CurrentSessionId() == s2);
    CHECK(f.service.Ask(s2, "第二个会话的问题", &error));
    CHECK(RequestRoles(f.platform->request).size() == 2);  // system + only this session's question
    f.platform->Reply("答2");
    CHECK(f.service.List().size() == 2);

    CHECK(f.service.CycleSession(1) == s1);  // wraps
    CHECK(f.service.CycleSession(-1) == s2);
    CHECK(f.service.SelectSession(s1) && f.service.CurrentSessionId() == s1);
    CHECK(!f.service.SelectSession(999));

    CHECK(f.service.DeleteSession(s1));
    CHECK(f.service.CurrentSessionId() == s2);
    CHECK(f.service.DeleteSession(s2));  // never zero sessions
    CHECK(f.service.List().size() == 1 && f.service.List()[0].message_count == 0);
}

void TestSessionLimit() {
    Fixture f;
    for (size_t i = 1; i < ai::kMaxSessions; ++i) CHECK(f.service.CreateSession() > 0);
    CHECK(f.service.List().size() == ai::kMaxSessions);
    CHECK(f.service.CreateSession() == -1);
}

void TestAskRejections() {
    std::string error;
    {
        Fixture f("");  // no key
        CHECK(!f.service.Ask(f.service.CurrentSessionId(), "hi", &error) && !error.empty());
        CHECK(!f.service.SetApiKey("has space") && !f.service.SetApiKey("   ") && !f.service.SetApiKey("a\"b"));
        CHECK(f.service.SetApiKey("  sk-new  ") && f.platform->stored_key == "sk-new");
        CHECK(f.service.Ask(f.service.CurrentSessionId(), "hi", &error));
    }
    Fixture f;
    const int id = f.service.CurrentSessionId();
    CHECK(!f.service.Ask(id, "   ", &error));
    CHECK(!f.service.Ask(id, std::string(ai::kMaxQuestionBytes + 1, 'x'), &error));
    CHECK(!f.service.Ask(12345, "hi", &error));
    CHECK(f.platform->posts == 0);
    CHECK(f.service.Ask(id, "first", &error));
    CHECK(!f.service.Ask(id, "second while pending", &error));  // one request at a time
    const int other = f.service.CreateSession();
    CHECK(!f.service.Ask(other, "other session while busy", &error));
    CHECK(f.platform->posts == 1);
}

void TestFailureKeepsContextClean() {
    Fixture f;
    std::string error;
    const int id = f.service.CurrentSessionId();
    CHECK(f.service.Ask(id, "q1", &error));
    f.platform->Fail(401, R"({"error":{"message":"Authentication Fails"}})");
    ai::Session snap;
    CHECK(f.service.Snapshot(id, &snap));
    CHECK(snap.messages.empty() && !snap.pending && snap.title.empty());  // unanswered question forgotten
    CHECK(snap.last_error.find("401") != std::string::npos);

    CHECK(f.service.Ask(id, "q2", &error));  // can retry; error cleared
    CHECK(f.service.Snapshot(id, &snap) && snap.last_error.empty());
    f.platform->Fail(0, "", "ESP_ERR_HTTP_CONNECT");
    CHECK(f.service.Snapshot(id, &snap) && snap.last_error.find("ESP_ERR_HTTP_CONNECT") != std::string::npos);

    CHECK(f.service.Ask(id, "q2b", &error));
    f.platform->Fail(403, "<html>Access Denied</html>");  // a web filter's page instead of JSON
    CHECK(f.service.Snapshot(id, &snap) && snap.last_error == "请求失败 (HTTP 403)");

    CHECK(f.service.Ask(id, "q3", &error));
    f.platform->Reply("a3");
    CHECK(f.service.Snapshot(id, &snap) && snap.messages.size() == 2 && snap.last_error.empty());
    const auto roles = RequestRoles(f.platform->request);
    CHECK((roles == std::vector<std::string>{"system", "user"}));  // failed questions were not sent as context
}

void TestDeleteWhilePending() {
    Fixture f;
    std::string error;
    const int s1 = f.service.CurrentSessionId();
    const int s2 = f.service.CreateSession();
    CHECK(f.service.SelectSession(s1));
    CHECK(f.service.Ask(s1, "q", &error));
    CHECK(f.service.DeleteSession(s1));
    f.platform->Reply("late answer");  // must not crash or resurrect it
    CHECK(f.service.List().size() == 1 && f.service.List()[0].id == s2);
    CHECK(f.service.Ask(s2, "next", &error));  // busy flag was released
}

void TestHistoryIsBounded() {
    Fixture f;
    std::string error;
    const int id = f.service.CurrentSessionId();
    for (size_t i = 0; i < ai::kMaxStoredMessages; ++i) {  // 2 messages per round
        CHECK(f.service.Ask(id, "q" + std::to_string(i), &error));
        f.platform->Reply("a" + std::to_string(i));
    }
    ai::Session snap;
    CHECK(f.service.Snapshot(id, &snap));
    CHECK(snap.messages.size() <= ai::kMaxStoredMessages);
    CHECK(snap.messages.front().role == Message::User && snap.messages.back().role == Message::Assistant);
    CHECK(snap.messages.back().text == "a" + std::to_string(ai::kMaxStoredMessages - 1));
}

void TestSpeechHelpers() {
    CHECK(ai::Base64Encode("") == "" && ai::Base64Encode("f") == "Zg==" && ai::Base64Encode("fo") == "Zm8=");
    CHECK(ai::Base64Encode("foo") == "Zm9v" && ai::Base64Encode("foobar") == "Zm9vYmFy");
    CHECK(ai::Base64Encode(std::string("\xff\xfe\x00", 3)) == "//4A");

    CHECK(ai::IsUsableTranscript("今天天气怎么样？") && ai::IsUsableTranscript("hi") && ai::IsUsableTranscript("你好"));
    CHECK(!ai::IsUsableTranscript("嗯。") && !ai::IsUsableTranscript("") && !ai::IsUsableTranscript("。。。 ") &&
          !ai::IsUsableTranscript("..."));  // silence comes back as a lone filler

    // The request body around the audio is valid JSON of the shape Qwen3-ASR expects.
    ai::ChatConfig config;
    std::string prefix, suffix;
    ai::SpeechBodyParts(config, &prefix, &suffix);
    cJSON* root = cJSON_Parse((prefix + "QUJD" + suffix).c_str());
    CHECK(root != nullptr);
    CHECK(std::string(cJSON_GetObjectItemCaseSensitive(root, "model")->valuestring) == "qwen3-asr-flash");
    cJSON* part = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "messages"), 0), "content"), 0);
    CHECK(std::string(cJSON_GetObjectItemCaseSensitive(part, "type")->valuestring) == "input_audio");
    CHECK(std::string(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(part, "input_audio"), "data")
                          ->valuestring) == "data:audio/wav;base64,QUJD");
    cJSON_Delete(root);

    // Files: base64 between prefix and suffix, and the file is gone afterwards.
    char path[] = "/tmp/test_ai_wav_XXXXXX";
    int fd = mkstemp(path);
    CHECK(write(fd, "foo", 3) == 3);
    close(fd);
    ai::ChatPlatform::HttpRequest req;
    req.body_file = path;
    req.body_prefix = "<";
    req.body_suffix = ">";
    CHECK(ai::BuildFileBody(&req) && req.body == "<Zm9v>");
    CHECK(access(path, F_OK) != 0);
    req.body_file = "/tmp/does_not_exist.wav";
    CHECK(!ai::BuildFileBody(&req));
}

void TestVoiceQuestion() {
    Fixture f;
    std::string error;
    const int id = f.service.CurrentSessionId();
    CHECK(f.service.VoiceReady(id, &error));
    CHECK(f.service.AskVoice(id, "/tmp/q.wav", &error));
    // 1. the recording goes to the speech model, with the same key and endpoint as chat
    CHECK(f.platform->asr_requests == 1 && f.platform->posts == 0 && f.platform->wav_path == "/tmp/q.wav");
    CHECK(f.platform->url == "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
    CHECK(f.platform->api_key == "sk-test");
    CHECK(f.platform->asr_prefix.find("qwen3-asr-flash") != std::string::npos);
    ai::Session snap;
    CHECK(f.service.Snapshot(id, &snap) && snap.pending && snap.transcribing && snap.messages.empty());
    CHECK(!f.service.Ask(id, "typed meanwhile", &error));           // busy while transcribing

    f.platform->Reply("  明天会下雨吗  ");                          // transcript = assistant content
    // 2. the transcript is an ordinary question
    CHECK(f.platform->posts == 1);
    CHECK(f.service.Snapshot(id, &snap) && snap.pending && !snap.transcribing);
    std::vector<std::string> contents;
    RequestRoles(f.platform->request, &contents);
    CHECK(contents.back() == "明天会下雨吗");
    f.platform->Reply("会");
    CHECK(f.service.Snapshot(id, &snap) && snap.messages.size() == 2 && snap.title == "明天会下雨吗");
}

void TestVoiceQuestionFailures() {
    std::string error;
    {
        Fixture f;  // silence is transcribed as a lone filler word
        const int id = f.service.CurrentSessionId();
        CHECK(f.service.AskVoice(id, "/tmp/q.wav", &error));
        f.platform->Reply("嗯。");
        ai::Session snap;
        CHECK(f.service.Snapshot(id, &snap) && !snap.pending && snap.last_error.find("没听清") != std::string::npos);
        CHECK(f.platform->posts == 0 && snap.messages.empty());
        CHECK(f.service.Ask(id, "typed works after a failure", &error));  // busy released
    }
    {
        Fixture f;  // bad key
        const int id = f.service.CurrentSessionId();
        CHECK(f.service.AskVoice(id, "/tmp/q.wav", &error));
        f.platform->Fail(401, R"({"error":{"message":"Incorrect API key provided."}})");
        ai::Session snap;
        CHECK(f.service.Snapshot(id, &snap) && !snap.pending);
        CHECK(snap.last_error.find("语音识别失败") != std::string::npos && snap.last_error.find("401") != std::string::npos);
        CHECK(f.platform->posts == 0);
    }
    {
        Fixture f;  // network
        const int id = f.service.CurrentSessionId();
        CHECK(f.service.AskVoice(id, "/tmp/q.wav", &error));
        f.platform->Fail(0, "", "ESP_ERR_HTTP_CONNECT");
        ai::Session snap;
        CHECK(f.service.Snapshot(id, &snap) && snap.last_error.find("ESP_ERR_HTTP_CONNECT") != std::string::npos);
    }
    {
        Fixture f;  // deleted while transcribing
        const int id = f.service.CurrentSessionId();
        CHECK(f.service.AskVoice(id, "/tmp/q.wav", &error));
        CHECK(f.service.DeleteSession(id));
        f.platform->Reply("你好吗");
        CHECK(f.platform->posts == 0 && f.service.Ask(f.service.CurrentSessionId(), "next", &error));
    }
    {
        Fixture f("");  // no key: refused up front, before recording
        CHECK(!f.service.VoiceReady(f.service.CurrentSessionId(), &error) && error.find("百炼 API Key") != std::string::npos);
        CHECK(f.service.SetApiKey("sk-ws-test.key.value") && f.service.VoiceReady(f.service.CurrentSessionId(), &error));
    }
}

void TestChangeNotifications() {
    Fixture f;
    std::string error;
    const int before = f.changes;
    f.service.Ask(f.service.CurrentSessionId(), "q", &error);
    CHECK(f.changes > before);  // pending state is visible
    const int mid = f.changes;
    f.platform->Reply("a");
    CHECK(f.changes > mid);     // and so is the answer
}

}  // namespace

int main() {
    TestRequestJson();
    TestParseResponse();
    TestPlainTextAndTitle();
    TestTrimHistory();
    TestSessionKeepsContext();
    TestSessionsAreIsolated();
    TestSessionLimit();
    TestAskRejections();
    TestFailureKeepsContextClean();
    TestDeleteWhilePending();
    TestHistoryIsBounded();
    TestSpeechHelpers();
    TestVoiceQuestion();
    TestVoiceQuestionFailures();
    TestChangeNotifications();
    if (g_failed) {
        fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("test_ai_chat: all passed\n");
    return 0;
}
