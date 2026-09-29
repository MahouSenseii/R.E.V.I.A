#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::planning
{

using WallClock = std::chrono::system_clock;

struct ReminderRequest
{
    // What to say, with the user's "my" already turned into "your". For a check, the
    // question to look up, as asked.
    std::string text;
    WallClock::time_point due;
    bool timer = false;
    // "10 minutes", for a timer's announcement.
    std::string timerLength;
    // Zero for a one-off. Otherwise the reminder comes back this much later each time
    // it is delivered ("every 30 minutes", "every day at 9am"), until cancelled.
    std::chrono::seconds repeatEvery{0};
    // A check rather than a reminder: when it is due she looks the question up and
    // says what she found, instead of saying the text back.
    bool check = false;

    [[nodiscard]] bool Repeats() const { return repeatEvery.count() > 0; }
};

struct Reminder
{
    std::uint64_t id = 0;
    ReminderRequest request;
};

enum class ReminderParse
{
    // Not a reminder: the input goes on to the model as usual.
    NotAReminder,
    Parsed,
    // Clearly a reminder that cannot be set; the error says what is missing.
    Invalid
};

// Reads "remind me in 10 minutes to stretch", "remind me to call Sam at 3pm",
// "set a timer for 5 minutes" and "/remind 10m stretch" without the model, so a reminder
// never depends on one and is never promised by one that cannot keep it. Also the
// repeating forms ("remind me every 30 minutes to stretch", "every day at 9am take my
// pills") and checks ("check the weather in Boston every hour", "/check 20m is the
// build green"), which are reminders whose delivery is a lookup.
ReminderParse ParseReminderRequest(const std::string& input, WallClock::time_point now,
    ReminderRequest& out, std::string& outError);
// A repeat shorter than this would be nagging; a check shorter would be hammering.
constexpr std::chrono::seconds ShortestRepeat{60};
constexpr std::chrono::seconds ShortestCheckRepeat{5 * 60};

// "3:05 PM", "tomorrow 9:00 AM" or "Sep 30 9:00 AM", in local time.
[[nodiscard]] std::string DescribeWhen(WallClock::time_point when, WallClock::time_point now);
// "40 s", "12 min", "2 h 5 min".
[[nodiscard]] std::string DescribeSpan(WallClock::duration span);
// "every 30 min", "every day", "every 2 h". Empty for a one-off.
[[nodiscard]] std::string DescribeRepeat(const ReminderRequest& request);
// What she says when one is due: "Reminder: stretch." or "Your timer for 10 minutes is done."
// A check has no announcement: its delivery is the lookup's answer.
[[nodiscard]] std::string Announcement(const ReminderRequest& request);
// How a list names it: "stretch", "timer for 10 minutes", "check: the weather (every hour)".
[[nodiscard]] std::string Label(const ReminderRequest& request);

// Pending reminders, saved after every change so they survive a restart. One missed
// while Revia was closed is delivered, late, the next time she runs.
class ReminderBook
{
public:
    static constexpr std::size_t MaximumPending = 100;

    // Loads what was saved. Without it the book still works, in memory only.
    bool Initialize(const std::filesystem::path& path, std::string& outError);

    // Empty when the book is full. A reminder that could not be saved is still kept,
    // with the reason in outError.
    std::optional<Reminder> Add(const ReminderRequest& request, std::string& outError);
    // Removes and returns everything due by `now`, earliest first. A repeating one is
    // put back for its next time past `now` (one missed while she was closed is
    // delivered once, late, not once per missed interval), under the same id.
    std::vector<Reminder> TakeDue(WallClock::time_point now);
    // Earliest first; the position is the number /reminders shows.
    [[nodiscard]] std::vector<Reminder> Pending() const;
    // `number` counts from 1 in Pending() order.
    std::optional<Reminder> Cancel(std::size_t number);
    std::size_t Clear();

private:
    bool SaveLocked(std::string& outError) const;

    mutable std::mutex mutex;
    std::filesystem::path path;
    // Kept sorted by due time.
    std::vector<Reminder> reminders;
    std::uint64_t nextId = 1;
};

} // namespace revia::planning
