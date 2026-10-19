#include "common/reminders.h"

#include <cJSON.h>

#include <cstdio>
#include <cstring>

namespace reminders {

// ============================================================
// Calendar helpers
// ============================================================

bool IsLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int DaysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    return kDays[month - 1] + (month == 2 && IsLeapYear(year) ? 1 : 0);
}

int Weekday(int year, int month, int day) {
    // Sakamoto's method.
    static const int kOffset[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) year -= 1;
    return (year + year / 4 - year / 100 + year / 400 + kOffset[month - 1] + day) % 7;
}

bool IsValidDate(int year, int month, int day) {
    return year >= kMinYear && year <= kMaxYear && month >= 1 && month <= 12 && day >= 1 &&
           day <= DaysInMonth(year, month);
}

std::string DateKey(int year, int month, int day) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
    return buf;
}

bool ParseDateKey(const std::string& key, int* year, int* month, int* day) {
    if (key.size() != 10 || key[4] != '-' || key[7] != '-') return false;
    for (size_t i = 0; i < key.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (key[i] < '0' || key[i] > '9') return false;
    }
    const int y = std::stoi(key.substr(0, 4));
    const int m = std::stoi(key.substr(5, 2));
    const int d = std::stoi(key.substr(8, 2));
    if (!IsValidDate(y, m, d)) return false;
    if (year) *year = y;
    if (month) *month = m;
    if (day) *day = d;
    return true;
}

std::string NormalizeText(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) c = ' ';
    }
    const size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return "";
    const size_t last = out.find_last_not_of(' ');
    return out.substr(first, last - first + 1);
}

bool IsValidUtf8(const std::string& text) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(text.data());
    const size_t n = text.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = s[i];
        size_t len;
        unsigned cp;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        const unsigned min_cp = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
        if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

// ============================================================
// File persistence
// ============================================================

namespace {

class FilePersistence : public Persistence {
public:
    explicit FilePersistence(std::string path) : path_(std::move(path)) {}

    bool Read(std::string* out) override {
        FILE* f = fopen(path_.c_str(), "rb");
        if (!f) return false;
        out->clear();
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
        fclose(f);
        return true;
    }

    bool Write(const std::string& data) override {
        const std::string tmp = path_ + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return false;
        const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
        const bool closed = fclose(f) == 0;
        if (!ok || !closed) {
            remove(tmp.c_str());
            return false;
        }
        remove(path_.c_str());  // SPIFFS rename does not replace an existing file
        if (rename(tmp.c_str(), path_.c_str()) != 0) {
            remove(tmp.c_str());
            return false;
        }
        return true;
    }

private:
    std::string path_;
};

}  // namespace

std::unique_ptr<Persistence> NewFilePersistence(const std::string& path) {
    return std::make_unique<FilePersistence>(path);
}

// ============================================================
// Store
// ============================================================

const char* SetResultName(SetResult result) {
    switch (result) {
        case SetResult::Ok: return "ok";
        case SetResult::BadDate: return "bad_date";
        case SetResult::TooMany: return "too_many";
        case SetResult::TextTooLong: return "text_too_long";
        case SetResult::BadText: return "bad_text";
        case SetResult::TooManyDates: return "too_many_dates";
        case SetResult::SaveFailed: return "save_failed";
    }
    return "unknown";
}

Store& Store::Shared() {
    static Store store;
    return store;
}

void Store::SetPersistence(std::unique_ptr<Persistence> persistence) {
    std::lock_guard<std::mutex> lock(mu_);
    persistence_ = std::move(persistence);
    data_.clear();
    loaded_ = false;
}

void Store::SetOnChange(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mu_);
    on_change_ = std::move(callback);
}

void Store::LoadLocked() {
    if (loaded_) return;
    loaded_ = true;
    data_.clear();
    std::string json;
    if (persistence_ && persistence_->Read(&json)) Parse(json, &data_);
}

void Store::Parse(const std::string& json, Snapshot* out) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }
    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, root) {
        if (!entry->string || !cJSON_IsArray(entry) || !ParseDateKey(entry->string, nullptr, nullptr, nullptr)) continue;
        if (static_cast<int>(out->size()) >= kMaxDates) break;
        std::vector<std::string> items;
        cJSON* text = nullptr;
        cJSON_ArrayForEach(text, entry) {
            if (!cJSON_IsString(text) || !IsValidUtf8(text->valuestring)) continue;
            std::string value = NormalizeText(text->valuestring);
            if (value.empty() || static_cast<int>(value.size()) > kMaxTextBytes) continue;
            if (static_cast<int>(items.size()) >= kMaxPerDay) break;
            items.push_back(std::move(value));
        }
        if (!items.empty()) (*out)[entry->string] = std::move(items);
    }
    cJSON_Delete(root);
}

std::string Store::Serialize(const Snapshot& data) {
    cJSON* root = cJSON_CreateObject();
    for (const auto& [key, items] : data) {
        cJSON* list = cJSON_AddArrayToObject(root, key.c_str());
        for (const auto& text : items) cJSON_AddItemToArray(list, cJSON_CreateString(text.c_str()));
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text ? text : "{}";
    if (text) cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

std::vector<std::string> Store::Get(int year, int month, int day) {
    std::lock_guard<std::mutex> lock(mu_);
    LoadLocked();
    auto it = data_.find(DateKey(year, month, day));
    return it == data_.end() ? std::vector<std::string>() : it->second;
}

bool Store::Has(int year, int month, int day) {
    std::lock_guard<std::mutex> lock(mu_);
    LoadLocked();
    return data_.count(DateKey(year, month, day)) > 0;
}

unsigned Store::DaysWithReminders(int year, int month) {
    std::lock_guard<std::mutex> lock(mu_);
    LoadLocked();
    unsigned mask = 0;
    const int days = DaysInMonth(year, month);
    for (int d = 1; d <= days; ++d) {
        if (data_.count(DateKey(year, month, d))) mask |= 1u << d;
    }
    return mask;
}

Store::Snapshot Store::All() {
    std::lock_guard<std::mutex> lock(mu_);
    LoadLocked();
    return data_;
}

SetResult Store::Set(int year, int month, int day, const std::vector<std::string>& items) {
    if (!IsValidDate(year, month, day)) return SetResult::BadDate;

    std::vector<std::string> clean;
    for (const auto& raw : items) {
        if (!IsValidUtf8(raw)) return SetResult::BadText;
        std::string text = NormalizeText(raw);
        if (text.empty()) continue;
        if (static_cast<int>(text.size()) > kMaxTextBytes) return SetResult::TextTooLong;
        clean.push_back(std::move(text));
    }
    if (static_cast<int>(clean.size()) > kMaxPerDay) return SetResult::TooMany;

    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(mu_);
        LoadLocked();
        const std::string key = DateKey(year, month, day);
        const auto existing = data_.find(key);
        const bool had = existing != data_.end();
        if (!had && clean.empty()) return SetResult::Ok;  // nothing to do
        if (!had && static_cast<int>(data_.size()) >= kMaxDates) return SetResult::TooManyDates;

        const std::vector<std::string> previous = had ? existing->second : std::vector<std::string>();
        if (clean.empty()) {
            data_.erase(key);
        } else {
            data_[key] = clean;
        }
        if (persistence_ && !persistence_->Write(Serialize(data_))) {
            if (had) {
                data_[key] = previous;
            } else {
                data_.erase(key);
            }
            return SetResult::SaveFailed;
        }
        notify = on_change_;
    }
    if (notify) notify();
    return SetResult::Ok;
}

}  // namespace reminders
