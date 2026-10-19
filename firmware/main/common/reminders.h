/**
 * @file reminders.h
 * @brief Calendar reminders: per-date text lists, calendar date helpers and persistence.
 *
 * Hardware-free so the firmware and the simulator share it. Storage goes through the small
 * Persistence interface (one JSON document): the firmware and the simulator both use
 * NewFilePersistence(), only the path differs. Dates are keyed "YYYY-MM-DD".
 */

#ifndef COMMON_REMINDERS_H
#define COMMON_REMINDERS_H

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace reminders {

constexpr int kMinYear = 2000;
constexpr int kMaxYear = 2099;
constexpr int kMaxPerDay = 6;       ///< reminders on one date
constexpr int kMaxTextBytes = 96;   ///< UTF-8 bytes per reminder (32 Chinese characters)
constexpr int kMaxDates = 100;      ///< dates that may hold reminders at once

// ---- calendar helpers (proleptic Gregorian) ----

bool IsLeapYear(int year);
int DaysInMonth(int year, int month);  ///< 0 for an invalid month
int Weekday(int year, int month, int day);  ///< 0 = Sunday ... 6 = Saturday
bool IsValidDate(int year, int month, int day);  ///< also enforces kMinYear..kMaxYear

std::string DateKey(int year, int month, int day);  ///< "2026-10-19"
bool ParseDateKey(const std::string& key, int* year, int* month, int* day);  ///< strict, validated

/** Trim, turn control characters/newlines into spaces. Empty result means "drop this entry". */
std::string NormalizeText(const std::string& text);
bool IsValidUtf8(const std::string& text);

// ---- persistence ----

/** Where the JSON document lives. Implementations must be safe to call from any task. */
class Persistence {
public:
    virtual ~Persistence() = default;
    /** @return false if nothing is stored yet or it cannot be read. */
    virtual bool Read(std::string* out) = 0;
    virtual bool Write(const std::string& data) = 0;
};

/** Plain stdio file (write to "<path>.tmp", then rename). Used on SPIFFS and in the simulator. */
std::unique_ptr<Persistence> NewFilePersistence(const std::string& path);

// ---- store ----

enum class SetResult {
    Ok,
    BadDate,
    TooMany,      ///< more than kMaxPerDay reminders
    TextTooLong,  ///< a reminder exceeds kMaxTextBytes
    BadText,      ///< not valid UTF-8
    TooManyDates, ///< would exceed kMaxDates
    SaveFailed,   ///< could not be written; the change is rolled back
};

const char* SetResultName(SetResult result);  ///< stable code for the web API

/** Thread-safe map date -> reminders, loaded lazily from the persistence on first use. */
class Store {
public:
    using Snapshot = std::map<std::string, std::vector<std::string>>;

    /** The instance shared by the device UI and the web handlers. */
    static Store& Shared();

    /** Install the persistence and drop anything loaded so far (re-read on next use). */
    void SetPersistence(std::unique_ptr<Persistence> persistence);

    /** Called after every successful change, outside the lock (any task). */
    void SetOnChange(std::function<void()> callback);

    std::vector<std::string> Get(int year, int month, int day);
    bool Has(int year, int month, int day);
    /** Bit d (1..31) set when day d of the month has reminders. */
    unsigned DaysWithReminders(int year, int month);
    Snapshot All();

    /** Replace the reminders of one date. Entries are normalised; an empty list deletes the date. */
    SetResult Set(int year, int month, int day, const std::vector<std::string>& items);

private:
    void LoadLocked();
    static std::string Serialize(const Snapshot& data);
    static void Parse(const std::string& json, Snapshot* out);

    std::mutex mu_;
    std::unique_ptr<Persistence> persistence_;
    Snapshot data_;
    bool loaded_ = false;
    std::function<void()> on_change_;
};

}  // namespace reminders

#endif  // COMMON_REMINDERS_H
