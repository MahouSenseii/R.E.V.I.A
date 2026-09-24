#include "Planning/reminders.h"

#include "Core/utf8.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>

namespace revia::planning
{

namespace
{
constexpr auto LongestReminder = std::chrono::hours(24 * 30);
constexpr std::size_t LongestText = 200;

struct Word
{
    // Lowercase, without trailing punctuation: what is matched.
    std::string lower;
    // As typed: what goes into the reminder.
    std::string original;
};

std::vector<Word> Words(const std::string& text)
{
    std::vector<Word> words;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token)
    {
        std::string lower;
        for (const unsigned char character : token)
        {
            lower.push_back(static_cast<char>(std::tolower(character)));
        }
        while (!lower.empty() && std::string(",.!?;:").find(lower.back()) != std::string::npos)
        {
            lower.pop_back();
        }
        words.push_back({lower, token});
    }
    return words;
}

bool Is(const std::vector<Word>& words, const std::size_t index, const char* word)
{
    return index < words.size() && words[index].lower == word;
}

bool IsAny(const std::vector<Word>& words, const std::size_t index,
    std::initializer_list<const char*> options)
{
    return std::any_of(options.begin(), options.end(),
        [&](const char* option) { return Is(words, index, option); });
}

std::optional<double> Digits(const std::string& word)
{
    if (word.empty() || word.size() > 6) return std::nullopt;
    bool dot = false;
    for (const char character : word)
    {
        if (character == '.' && !dot) { dot = true; continue; }
        if (std::isdigit(static_cast<unsigned char>(character)) == 0) return std::nullopt;
    }
    if (word.front() == '.' || word.back() == '.') return std::nullopt;
    return std::stod(word);
}

std::optional<double> NumberWord(const std::string& word)
{
    static const std::map<std::string, int> numbers = {
        {"a", 1}, {"an", 1}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4},
        {"five", 5}, {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10},
        {"eleven", 11}, {"twelve", 12}, {"fifteen", 15}, {"twenty", 20}, {"thirty", 30},
        {"forty", 40}, {"fifty", 50}, {"sixty", 60}, {"ninety", 90}};
    if (const auto digits = Digits(word)) return digits;
    if (const auto found = numbers.find(word); found != numbers.end()) return found->second;
    // "twenty-five"
    if (const std::size_t hyphen = word.find('-'); hyphen != std::string::npos)
    {
        const auto tens = numbers.find(word.substr(0, hyphen));
        const auto ones = numbers.find(word.substr(hyphen + 1));
        if (tens != numbers.end() && ones != numbers.end() && tens->second >= 20 &&
            tens->second % 10 == 0 && ones->second < 10)
        {
            return tens->second + ones->second;
        }
    }
    return std::nullopt;
}

std::optional<double> UnitSeconds(const std::string& word)
{
    static const std::map<std::string, double> units = {
        {"s", 1}, {"sec", 1}, {"secs", 1}, {"second", 1}, {"seconds", 1},
        {"m", 60}, {"min", 60}, {"mins", 60}, {"minute", 60}, {"minutes", 60},
        {"h", 3600}, {"hr", 3600}, {"hrs", 3600}, {"hour", 3600}, {"hours", 3600},
        {"d", 86400}, {"day", 86400}, {"days", 86400}};
    const auto found = units.find(word);
    return found == units.end() ? std::nullopt : std::optional<double>(found->second);
}

// "10m", "90s", "1h30m".
std::optional<double> CompactDuration(const std::string& word)
{
    double total = 0;
    std::size_t index = 0;
    while (index < word.size())
    {
        const std::size_t numberStart = index;
        while (index < word.size() &&
            (std::isdigit(static_cast<unsigned char>(word[index])) != 0 || word[index] == '.'))
        {
            ++index;
        }
        const std::size_t unitStart = index;
        while (index < word.size() && std::isalpha(static_cast<unsigned char>(word[index])) != 0)
        {
            ++index;
        }
        const auto number = Digits(word.substr(numberStart, unitStart - numberStart));
        const auto unit = UnitSeconds(word.substr(unitStart, index - unitStart));
        if (!number || !unit) return std::nullopt;
        total += *number * *unit;
    }
    return total > 0 ? std::optional<double>(total) : std::nullopt;
}

// A duration from words[start]: "10 minutes", "an hour and a half", "1 hour 20 minutes",
// "half an hour", "1h30m". `consumed` is how many words it took.
std::optional<double> ReadDuration(
    const std::vector<Word>& words, const std::size_t start, std::size_t& consumed)
{
    double total = 0;
    double lastUnit = 0;
    std::size_t index = start;
    while (index < words.size())
    {
        std::size_t next = index;
        if (lastUnit > 0 && Is(words, next, "and")) ++next;
        if (lastUnit > 0 && next > index && Is(words, next, "a") && Is(words, next + 1, "half"))
        {
            total += lastUnit / 2;
            index = next + 2;
            continue;
        }
        if (Is(words, next, "half") && IsAny(words, next + 1, {"a", "an"}) &&
            next + 2 < words.size())
        {
            if (const auto unit = UnitSeconds(words[next + 2].lower))
            {
                total += *unit / 2;
                lastUnit = *unit;
                index = next + 3;
                continue;
            }
        }
        if (next >= words.size()) break;
        if (const auto compact = CompactDuration(words[next].lower))
        {
            total += *compact;
            lastUnit = 60;
            index = next + 1;
            continue;
        }
        std::optional<double> number = NumberWord(words[next].lower);
        std::size_t unitIndex = next + 1;
        // "twenty five minutes"
        if (number && *number >= 20 && std::fmod(*number, 10) == 0 && unitIndex < words.size())
        {
            if (const auto ones = NumberWord(words[unitIndex].lower);
                ones && *ones >= 1 && *ones <= 9 && !Digits(words[unitIndex].lower))
            {
                *number += *ones;
                ++unitIndex;
            }
        }
        if (!number || unitIndex >= words.size()) break;
        const auto unit = UnitSeconds(words[unitIndex].lower);
        if (!unit) break;
        total += *number * *unit;
        lastUnit = *unit;
        index = unitIndex + 1;
    }
    consumed = index - start;
    return consumed > 0 ? std::optional<double>(total) : std::nullopt;
}

struct ClockTime
{
    int hour = 0;
    int minute = 0;
    // False for "at 3": morning or afternoon is still to be decided.
    bool exact = true;
};

// "3pm", "3:30 pm", "15:30", "noon", "3 o'clock".
std::optional<ClockTime> ReadClock(
    const std::vector<Word>& words, const std::size_t start, std::size_t& consumed)
{
    if (start >= words.size()) return std::nullopt;
    const std::string& word = words[start].lower;
    consumed = 1;
    if (word == "noon" || word == "midday") return ClockTime{12, 0, true};
    if (word == "midnight") return ClockTime{0, 0, true};

    // Spoken: "five", "five thirty", "seven fifteen pm".
    if (const auto spokenHour = NumberWord(word);
        spokenHour && !Digits(word) && word != "a" && word != "an" &&
        *spokenHour >= 1 && *spokenHour <= 12)
    {
        int minute = 0;
        if (start + 1 < words.size())
        {
            if (const auto spokenMinute = NumberWord(words[start + 1].lower);
                spokenMinute && !Digits(words[start + 1].lower) && *spokenMinute >= 10 &&
                *spokenMinute <= 59)
            {
                minute = static_cast<int>(*spokenMinute);
                ++consumed;
                // "seven forty five"
                if (minute % 10 == 0 && minute >= 20 && start + 2 < words.size())
                {
                    if (const auto ones = NumberWord(words[start + 2].lower);
                        ones && !Digits(words[start + 2].lower) && *ones >= 1 && *ones <= 9 &&
                        words[start + 2].lower != "a" && words[start + 2].lower != "an")
                    {
                        minute += static_cast<int>(*ones);
                        ++consumed;
                    }
                }
            }
        }
        const int hour = static_cast<int>(*spokenHour);
        if (IsAny(words, start + consumed, {"am", "pm", "a.m", "p.m"}))
        {
            const bool afternoon = words[start + consumed].lower.front() == 'p';
            ++consumed;
            return ClockTime{hour % 12 + (afternoon ? 12 : 0), minute, true};
        }
        if (Is(words, start + consumed, "o'clock")) ++consumed;
        return ClockTime{hour, minute, false};
    }

    std::size_t index = 0;
    int hour = 0;
    while (index < word.size() && index < 2 && std::isdigit(static_cast<unsigned char>(word[index])) != 0)
    {
        hour = hour * 10 + (word[index] - '0');
        ++index;
    }
    if (index == 0) return std::nullopt;
    int minute = 0;
    bool hasMinutes = false;
    if (index < word.size() && (word[index] == ':' || word[index] == '.'))
    {
        if (index + 3 > word.size() ||
            std::isdigit(static_cast<unsigned char>(word[index + 1])) == 0 ||
            std::isdigit(static_cast<unsigned char>(word[index + 2])) == 0)
        {
            return std::nullopt;
        }
        minute = (word[index + 1] - '0') * 10 + (word[index + 2] - '0');
        hasMinutes = true;
        index += 3;
    }
    std::string suffix = word.substr(index);
    if (suffix.empty() && IsAny(words, start + 1, {"am", "pm", "a.m", "p.m"}))
    {
        suffix = words[start + 1].lower;
        ++consumed;
    }
    else if (suffix.empty() && Is(words, start + 1, "o'clock"))
    {
        ++consumed;
    }
    suffix.erase(std::remove(suffix.begin(), suffix.end(), '.'), suffix.end());
    if (minute > 59) return std::nullopt;
    if (suffix == "am" || suffix == "pm")
    {
        if (hour < 1 || hour > 12) return std::nullopt;
        return ClockTime{hour % 12 + (suffix == "pm" ? 12 : 0), minute, true};
    }
    if (!suffix.empty() || hour > 23) return std::nullopt;
    // A bare "at 5" reads as a time; a bare "at 2024" or "at 0" does not.
    if (!hasMinutes && (hour == 0 || word.size() > 2)) return std::nullopt;
    return ClockTime{hour, minute, hour == 0 || hour > 12};
}

std::tm LocalTime(const WallClock::time_point when)
{
    const std::time_t time = WallClock::to_time_t(when);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return local;
}

std::optional<WallClock::time_point> AtLocal(
    const WallClock::time_point now, const int dayOffset, const int hour, const int minute)
{
    std::tm local = LocalTime(now);
    local.tm_mday += dayOffset;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    const std::time_t resolved = std::mktime(&local);
    if (resolved == static_cast<std::time_t>(-1)) return std::nullopt;
    return WallClock::from_time_t(resolved);
}

// "at 3pm", "at 9 tomorrow", "tomorrow at 9am", "today at noon".
std::optional<WallClock::time_point> ReadWhen(const std::vector<Word>& words,
    const std::size_t start, const WallClock::time_point now, std::size_t& consumed,
    std::string& outError)
{
    std::size_t index = start;
    int day = -1;
    bool evening = false;
    const auto readDay = [&]()
    {
        if (!IsAny(words, index, {"today", "tonight", "tomorrow"})) return;
        day = Is(words, index, "tomorrow") ? 1 : 0;
        evening = Is(words, index, "tonight");
        ++index;
    };
    readDay();
    if (Is(words, index, "at")) ++index;
    else if (day < 0) return std::nullopt;
    std::size_t clockWords = 0;
    const std::optional<ClockTime> clock = ReadClock(words, index, clockWords);
    if (!clock) return std::nullopt;
    index += clockWords;
    if (day < 0) readDay();
    consumed = index - start;

    // Without am or pm: the next time the clock shows it, or, on a named day, the
    // daytime reading ("tomorrow at 3" is the afternoon, "tonight at 8" the evening).
    std::vector<int> hours = {clock->hour};
    if (!clock->exact)
    {
        if (evening) hours = {clock->hour % 12 + 12};
        else if (day < 0) hours = {clock->hour % 12, clock->hour % 12 + 12};
        else if (clock->hour < 7) hours = {clock->hour + 12};
    }
    std::optional<WallClock::time_point> best;
    for (const int offset : day < 0 ? std::vector<int>{0, 1} : std::vector<int>{day})
    {
        for (const int hour : hours)
        {
            const auto candidate = AtLocal(now, offset, hour, clock->minute);
            if (candidate && *candidate > now && (!best || *candidate < *best)) best = candidate;
        }
    }
    if (!best) outError = "That time has already passed today.";
    return best;
}

std::string SpokenLength(const long long totalSeconds)
{
    const long long hours = totalSeconds / 3600;
    const long long minutes = totalSeconds % 3600 / 60;
    const long long seconds = totalSeconds % 60;
    std::string spoken;
    const auto add = [&spoken](const long long count, const char* unit)
    {
        if (count == 0) return;
        if (!spoken.empty()) spoken += " ";
        spoken += std::to_string(count) + " " + unit + (count == 1 ? "" : "s");
    };
    add(hours, "hour");
    add(minutes, "minute");
    add(seconds, "second");
    return spoken;
}

// The user's words, said back to them: "take my pills" becomes "take your pills".
std::string AddressedToUser(const std::vector<Word>& words, const std::size_t from,
    const std::size_t to)
{
    static const std::map<std::string, std::string> swaps = {
        {"my", "your"}, {"me", "you"}, {"mine", "yours"}, {"myself", "yourself"},
        {"i", "you"}, {"i'm", "you're"}, {"i've", "you've"}, {"i'll", "you'll"},
        {"i'd", "you'd"}, {"am", "are"}};
    std::string text;
    bool previousWasI = false;
    for (std::size_t index = from; index < to; ++index)
    {
        const Word& word = words[index];
        std::string out = word.original;
        const auto swap = swaps.find(word.lower);
        // "am" only after "I": "I am late", not "8 am".
        if (swap != swaps.end() && (word.lower != "am" || previousWasI))
        {
            // Keeps punctuation that followed the word.
            out = swap->second + word.original.substr(std::min(word.original.size(), word.lower.size()));
        }
        previousWasI = word.lower == "i";
        if (!text.empty()) text += " ";
        text += out;
    }
    while (!text.empty() && std::string(".!?,;:").find(text.back()) != std::string::npos)
    {
        text.pop_back();
    }
    return utf8::Prefix(text, LongestText);
}

ReminderParse Finish(ReminderRequest& request, const WallClock::time_point now,
    ReminderRequest& out, std::string& outError)
{
    if (request.due <= now)
    {
        outError = "That's no time at all. Tell me when, for example: in 10 minutes.";
        return ReminderParse::Invalid;
    }
    if (request.due - now > LongestReminder)
    {
        outError = "I can only hold reminders up to 30 days ahead.";
        return ReminderParse::Invalid;
    }
    out = std::move(request);
    return ReminderParse::Parsed;
}

WallClock::time_point After(const WallClock::time_point now, const double seconds)
{
    const double bounded = std::min(seconds, 400.0 * 86400.0);
    return now + std::chrono::duration_cast<WallClock::duration>(
        std::chrono::duration<double>(bounded));
}

// A timer: "set a timer for 10 minutes", "set a 5 minute timer for the pasta".
ReminderParse ReadTimer(const std::vector<Word>& words, std::size_t index,
    const WallClock::time_point now, ReminderRequest& out, std::string& outError)
{
    if (IsAny(words, index, {"set", "start"})) ++index;
    std::size_t consumed = 0;
    std::optional<double> seconds;
    std::size_t labelStart = 0;
    if (IsAny(words, index, {"a", "an"}) && Is(words, index + 1, "timer")) ++index;
    if (Is(words, index, "timer") && Is(words, index + 1, "for"))
    {
        seconds = ReadDuration(words, index + 2, consumed);
        labelStart = index + 2 + consumed;
    }
    else
    {
        // "a 5 minute timer": there the article belongs to the timer, not the number.
        std::size_t at = index;
        if (IsAny(words, at, {"a", "an"}) && at + 1 < words.size() &&
            (NumberWord(words[at + 1].lower) || CompactDuration(words[at + 1].lower)))
        {
            ++at;
        }
        seconds = ReadDuration(words, at, consumed);
        if (!seconds || !Is(words, at + consumed, "timer")) return ReminderParse::NotAReminder;
        labelStart = at + consumed + 1;
    }
    if (!seconds) return ReminderParse::NotAReminder;
    if (labelStart < words.size() && !IsAny(words, labelStart, {"for", "to", "called", "named"}))
    {
        return ReminderParse::NotAReminder;
    }
    ReminderRequest request;
    request.timer = true;
    request.timerLength = SpokenLength(std::llround(*seconds));
    if (labelStart + 1 < words.size())
    {
        request.text = AddressedToUser(words, labelStart + 1, words.size());
    }
    request.due = After(now, *seconds);
    return Finish(request, now, out, outError);
}

} // namespace

ReminderParse ParseReminderRequest(const std::string& input, const WallClock::time_point now,
    ReminderRequest& out, std::string& outError)
{
    std::vector<Word> words = Words(input);
    // "... please" and "... thanks" are not part of what to be reminded of.
    while (!words.empty() && (words.back().lower == "please" || words.back().lower == "thanks"))
    {
        words.pop_back();
    }
    std::size_t index = 0;
    if (IsAny(words, index, {"hey", "ok", "okay"}) && Is(words, index + 1, "revia")) index += 2;
    else if (Is(words, index, "revia")) ++index;
    while (IsAny(words, index, {"please", "can", "could", "would", "will", "you"})) ++index;
    if (index >= words.size()) return ReminderParse::NotAReminder;

    const bool command = Is(words, index, "/remind");
    const bool spoken = Is(words, index, "remind") && Is(words, index + 1, "me");
    if (!command && !spoken)
    {
        return ReadTimer(words, index, now, out, outError);
    }
    const std::size_t start = index + (command ? 1 : 2);
    const auto remainder = [&](std::size_t from)
    {
        if (IsAny(words, from, {"to", "that", "about"})) ++from;
        return AddressedToUser(words, from, words.size());
    };
    const char* usage = "Tell me when as well, for example: remind me in 20 minutes to "
        "stretch, or remind me at 3pm to call Sam.";

    ReminderRequest request;
    std::size_t consumed = 0;
    std::string whenError;
    // "in 10 minutes to ...", or for the command "10m ..."
    if (Is(words, start, "in") || (command && !Is(words, start, "at")))
    {
        const std::size_t from = start + (Is(words, start, "in") ? 1 : 0);
        if (const auto seconds = ReadDuration(words, from, consumed))
        {
            request.due = After(now, *seconds);
            request.text = remainder(from + consumed);
            if (request.text.empty())
            {
                request.timer = true;
                request.timerLength = SpokenLength(std::llround(*seconds));
            }
            return Finish(request, now, out, outError);
        }
    }
    // "at 3pm to ...", "tomorrow at 9 to ..."
    if (const auto due = ReadWhen(words, start, now, consumed, whenError))
    {
        request.due = *due;
        request.text = remainder(start + consumed);
        if (request.text.empty())
        {
            outError = "What should I remind you about?";
            return ReminderParse::Invalid;
        }
        return Finish(request, now, out, outError);
    }
    if (!whenError.empty())
    {
        outError = whenError;
        return ReminderParse::Invalid;
    }
    // "to call Sam at 3pm", "to check the oven in 10 minutes": the time comes last.
    if (IsAny(words, start, {"to", "that", "about"}))
    {
        // The leftmost place the rest reads as a time, so "tomorrow at 3" is not cut
        // down to "at 3".
        for (std::size_t at = start + 2; at < words.size(); ++at)
        {
            consumed = 0;
            if (Is(words, at, "in"))
            {
                const auto seconds = ReadDuration(words, at + 1, consumed);
                if (seconds && at + 1 + consumed == words.size())
                {
                    request.due = After(now, *seconds);
                    request.text = AddressedToUser(words, start + 1, at);
                    return Finish(request, now, out, outError);
                }
            }
            const auto due = ReadWhen(words, at, now, consumed, whenError);
            if (at + consumed == words.size() && (due || !whenError.empty()))
            {
                if (!due)
                {
                    outError = whenError;
                    return ReminderParse::Invalid;
                }
                request.due = *due;
                request.text = AddressedToUser(words, start + 1, at);
                return Finish(request, now, out, outError);
            }
            whenError.clear();
        }
        // "remind me to call mom" with no time is still clearly a reminder; saying "sure"
        // and never reminding would be worse than asking.
        if (Is(words, start, "to") || command)
        {
            outError = usage;
            return ReminderParse::Invalid;
        }
    }
    if (command)
    {
        outError = "Usage: /remind <when> <what>, for example /remind 10m stretch or "
            "/remind at 3pm call Sam.";
        return ReminderParse::Invalid;
    }
    // "remind me in a bit": a reminder, with a time she cannot read.
    if (IsAny(words, start, {"in", "at", "today", "tonight", "tomorrow"}))
    {
        outError = usage;
        return ReminderParse::Invalid;
    }
    return ReminderParse::NotAReminder;
}

std::string DescribeWhen(const WallClock::time_point when, const WallClock::time_point now)
{
    const std::tm local = LocalTime(when);
    const std::tm today = LocalTime(now);
    const int hour = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
    char minutes[3];
    std::snprintf(minutes, sizeof(minutes), "%02d", local.tm_min);
    const std::string clock = std::to_string(hour) + ":" + minutes +
        (local.tm_hour < 12 ? " AM" : " PM");
    if (local.tm_year == today.tm_year && local.tm_yday == today.tm_yday) return clock;
    const auto tomorrow = AtLocal(now, 1, 12, 0);
    if (tomorrow)
    {
        const std::tm next = LocalTime(*tomorrow);
        if (local.tm_year == next.tm_year && local.tm_yday == next.tm_yday)
        {
            return "tomorrow " + clock;
        }
    }
    char date[16];
    std::strftime(date, sizeof(date), "%b %d", &local);
    return std::string(date) + " " + clock;
}

std::string DescribeSpan(const WallClock::duration span)
{
    const long long seconds = std::max<long long>(0,
        std::chrono::duration_cast<std::chrono::seconds>(span).count());
    if (seconds < 60) return std::to_string(seconds) + " s";
    const long long minutes = (seconds + 30) / 60;
    if (minutes < 60) return std::to_string(minutes) + " min";
    if (minutes >= 48 * 60) return std::to_string((minutes + 720) / 1440) + " days";
    return std::to_string(minutes / 60) + " h" +
        (minutes % 60 == 0 ? std::string() : " " + std::to_string(minutes % 60) + " min");
}

std::string Announcement(const ReminderRequest& request)
{
    if (request.timer)
    {
        return request.text.empty()
            ? "Your timer for " + request.timerLength + " is done."
            : "Your timer for " + request.text + " is done (" + request.timerLength + ").";
    }
    return "Reminder: " + request.text + ".";
}

std::string Label(const ReminderRequest& request)
{
    if (!request.timer) return request.text;
    return "timer for " + (request.text.empty()
        ? request.timerLength
        : request.text + " (" + request.timerLength + ")");
}

bool ReminderBook::Initialize(const std::filesystem::path& file, std::string& outError)
{
    std::lock_guard lock(mutex);
    path = file;
    reminders.clear();
    std::ifstream stream(path);
    if (!stream) return true;
    try
    {
        const nlohmann::json data = nlohmann::json::parse(stream);
        nextId = std::max<std::uint64_t>(1, data.value<std::uint64_t>("next_id", 1));
        for (const nlohmann::json& entry : data.value("reminders", nlohmann::json::array()))
        {
            if (!entry.is_object() || reminders.size() >= MaximumPending) continue;
            Reminder reminder;
            reminder.id = entry.value<std::uint64_t>("id", 0);
            reminder.request.text = utf8::Prefix(entry.value("text", ""), LongestText);
            reminder.request.timer = entry.value("timer", false);
            reminder.request.timerLength = utf8::Prefix(entry.value("timer_length", ""), 64);
            reminder.request.due = WallClock::time_point(std::chrono::duration_cast<WallClock::duration>(
                std::chrono::milliseconds(entry.value<std::int64_t>("due_ms", 0))));
            if (reminder.id == 0 || (reminder.request.text.empty() && !reminder.request.timer))
            {
                continue;
            }
            nextId = std::max(nextId, reminder.id + 1);
            reminders.push_back(std::move(reminder));
        }
    }
    catch (const std::exception& error)
    {
        // Moved aside rather than overwritten by the next save.
        reminders.clear();
        stream.close();
        std::filesystem::path unreadable = path;
        unreadable += ".unreadable";
        std::error_code ignored;
        std::filesystem::rename(path, unreadable, ignored);
        outError = std::string("the file could not be read (") + error.what() + ")";
        return false;
    }
    std::sort(reminders.begin(), reminders.end(), [](const Reminder& left, const Reminder& right)
    {
        return left.request.due < right.request.due;
    });
    return true;
}

std::optional<Reminder> ReminderBook::Add(const ReminderRequest& request, std::string& outError)
{
    std::lock_guard lock(mutex);
    if (reminders.size() >= MaximumPending)
    {
        outError = "You already have " + std::to_string(MaximumPending) +
            " reminders waiting. Cancel some with /reminders first.";
        return std::nullopt;
    }
    Reminder reminder{nextId++, request};
    const auto position = std::upper_bound(reminders.begin(), reminders.end(), reminder,
        [](const Reminder& left, const Reminder& right)
        {
            return left.request.due < right.request.due;
        });
    reminders.insert(position, reminder);
    (void)SaveLocked(outError);
    return reminder;
}

std::vector<Reminder> ReminderBook::TakeDue(const WallClock::time_point now)
{
    std::lock_guard lock(mutex);
    const auto firstLater = std::find_if(reminders.begin(), reminders.end(),
        [now](const Reminder& reminder) { return reminder.request.due > now; });
    std::vector<Reminder> due(std::make_move_iterator(reminders.begin()),
        std::make_move_iterator(firstLater));
    if (!due.empty())
    {
        reminders.erase(reminders.begin(), firstLater);
        std::string ignored;
        (void)SaveLocked(ignored);
    }
    return due;
}

std::vector<Reminder> ReminderBook::Pending() const
{
    std::lock_guard lock(mutex);
    return reminders;
}

std::optional<Reminder> ReminderBook::Cancel(const std::size_t number)
{
    std::lock_guard lock(mutex);
    if (number == 0 || number > reminders.size()) return std::nullopt;
    Reminder removed = reminders[number - 1];
    reminders.erase(reminders.begin() + static_cast<std::ptrdiff_t>(number - 1));
    std::string ignored;
    (void)SaveLocked(ignored);
    return removed;
}

std::size_t ReminderBook::Clear()
{
    std::lock_guard lock(mutex);
    const std::size_t count = reminders.size();
    reminders.clear();
    std::string ignored;
    (void)SaveLocked(ignored);
    return count;
}

bool ReminderBook::SaveLocked(std::string& outError) const
{
    if (path.empty()) return true;
    nlohmann::json list = nlohmann::json::array();
    for (const Reminder& reminder : reminders)
    {
        list.push_back({
            {"id", reminder.id},
            {"text", reminder.request.text},
            {"timer", reminder.request.timer},
            {"timer_length", reminder.request.timerLength},
            {"due_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                reminder.request.due.time_since_epoch()).count()}});
    }
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::trunc);
        stream << nlohmann::json{{"version", 1}, {"next_id", nextId}, {"reminders", list}}.dump(2);
        stream.flush();
        if (!stream.good())
        {
            outError = "The reminder could not be saved, so it will not survive a restart.";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        outError = "The reminder could not be saved, so it will not survive a restart.";
        return false;
    }
    return true;
}

} // namespace revia::planning
