#include "Memory/temporalQuery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

namespace revia::memory
{
namespace
{
constexpr std::int64_t Minute = 60;
constexpr std::int64_t Hour = 60 * Minute;

std::tm LocalParts(const std::int64_t epochSeconds)
{
    const std::time_t value = static_cast<std::time_t>(epochSeconds);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &value);
#else
    localtime_r(&value, &parts);
#endif
    return parts;
}

// Local time throughout, with tm_isdst left at -1 so mktime resolves the offset itself.
// Doing this arithmetic in UTC would put "yesterday" an hour off twice a year, which is
// exactly when a recall failure is hardest to explain.
std::int64_t FromLocalParts(std::tm parts)
{
    parts.tm_isdst = -1;
    return static_cast<std::int64_t>(std::mktime(&parts));
}

std::int64_t StartOfDay(const std::int64_t epochSeconds)
{
    std::tm parts = LocalParts(epochSeconds);
    parts.tm_hour = 0;
    parts.tm_min = 0;
    parts.tm_sec = 0;
    return FromLocalParts(parts);
}

// Calendar arithmetic rather than adding 86400, because a day is not always 86400
// seconds. mktime normalises the overflow.
std::int64_t ShiftDays(const std::int64_t epochSeconds, const int days)
{
    std::tm parts = LocalParts(epochSeconds);
    parts.tm_mday += days;
    return FromLocalParts(parts);
}

std::int64_t ShiftMonths(const std::int64_t epochSeconds, const int months)
{
    std::tm parts = LocalParts(epochSeconds);
    parts.tm_mon += months;
    return FromLocalParts(parts);
}

std::int64_t TimeOfDay(const std::int64_t dayStart, const int hour)
{
    std::tm parts = LocalParts(dayStart);
    parts.tm_hour = hour;
    parts.tm_min = 0;
    parts.tm_sec = 0;
    return FromLocalParts(parts);
}

std::int64_t StartOfWeek(const std::int64_t epochSeconds)
{
    const std::tm parts = LocalParts(epochSeconds);
    // tm_wday is 0 for Sunday; weeks here start on Monday, which is how people mean
    // "last week" when they say it.
    const int offset = (parts.tm_wday + 6) % 7;
    return StartOfDay(ShiftDays(epochSeconds, -offset));
}

std::int64_t StartOfMonth(const std::int64_t epochSeconds)
{
    std::tm parts = LocalParts(epochSeconds);
    parts.tm_mday = 1;
    parts.tm_hour = 0;
    parts.tm_min = 0;
    parts.tm_sec = 0;
    return FromLocalParts(parts);
}

std::vector<std::string> Tokenize(const std::string& text)
{
    std::vector<std::string> tokens;
    std::string current;
    for (const unsigned char character : text)
    {
        if (std::isalnum(character) != 0)
        {
            current.push_back(static_cast<char>(std::tolower(character)));
            continue;
        }
        if (!current.empty())
        {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty())
    {
        tokens.push_back(current);
    }
    return tokens;
}

// Padded with spaces on both ends so a phrase lookup matches whole words only.
std::string Join(const std::vector<std::string>& tokens)
{
    std::string joined = " ";
    for (const std::string& token : tokens)
    {
        joined += token;
        joined += ' ';
    }
    return joined;
}

bool Contains(const std::string& padded, const char* phrase)
{
    return padded.find(std::string(" ") + phrase + " ") != std::string::npos;
}

// Returns -1 when the token is not a count. "a" and "an" count as one so "a week ago"
// resolves the same way "1 week ago" does.
int ParseCount(const std::string& token)
{
    if (token.empty())
    {
        return -1;
    }
    if (std::all_of(token.begin(), token.end(),
            [](const unsigned char character) { return std::isdigit(character) != 0; }))
    {
        if (token.size() > 4)
        {
            return -1;
        }
        const int value = std::stoi(token);
        return value > 0 && value <= 3650 ? value : -1;
    }
    static const std::array<std::pair<const char*, int>, 17> Words{{
        {"a", 1}, {"an", 1}, {"one", 1}, {"couple", 2}, {"two", 2}, {"few", 3},
        {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6}, {"seven", 7},
        {"eight", 8}, {"nine", 9}, {"ten", 10}, {"eleven", 11}, {"twelve", 12},
        {"several", 3}}};
    for (const auto& entry : Words)
    {
        if (token == entry.first)
        {
            return entry.second;
        }
    }
    return -1;
}

// "a few days ago" is not a claim about one specific day, and answering it with one is
// how a correct lookup still misses. Only these words widen the window; "three days ago"
// stays exact.
bool IsApproximateCount(const std::string& token)
{
    return token == "few" || token == "several" || token == "couple";
}

enum class Unit
{
    None,
    Minutes,
    Hours,
    Days,
    Weeks,
    Months,
    Years
};

Unit ParseUnit(const std::string& token)
{
    if (token == "minute" || token == "minutes" || token == "min" || token == "mins")
    {
        return Unit::Minutes;
    }
    if (token == "hour" || token == "hours" || token == "hr" || token == "hrs")
    {
        return Unit::Hours;
    }
    if (token == "day" || token == "days")
    {
        return Unit::Days;
    }
    if (token == "week" || token == "weeks")
    {
        return Unit::Weeks;
    }
    if (token == "month" || token == "months")
    {
        return Unit::Months;
    }
    if (token == "year" || token == "years")
    {
        return Unit::Years;
    }
    return Unit::None;
}

// 0 is Sunday, matching tm_wday. -1 means the token is not a weekday.
int ParseWeekday(const std::string& token)
{
    static const std::array<std::pair<const char*, int>, 14> Days{{
        {"sunday", 0}, {"sun", 0}, {"monday", 1}, {"mon", 1}, {"tuesday", 2},
        {"tue", 2}, {"wednesday", 3}, {"wed", 3}, {"thursday", 4}, {"thu", 4},
        {"friday", 5}, {"fri", 5}, {"saturday", 6}, {"sat", 6}}};
    for (const auto& entry : Days)
    {
        if (token == entry.first)
        {
            return entry.second;
        }
    }
    return -1;
}

// Scans for YYYY-MM-DD or YYYY/MM/DD anywhere in the raw text and returns the local day
// it names, or an invalid window.
TimeWindow ParseExplicitDate(const std::string& text)
{
    for (std::size_t index = 0; index + 10 <= text.size(); ++index)
    {
        const auto digit = [&](const std::size_t offset)
        {
            return std::isdigit(static_cast<unsigned char>(text[index + offset])) != 0;
        };
        const auto separator = [&](const std::size_t offset)
        {
            return text[index + offset] == '-' || text[index + offset] == '/';
        };
        if (!digit(0) || !digit(1) || !digit(2) || !digit(3) || !separator(4) ||
            !digit(5) || !digit(6) || !separator(7) || !digit(8) || !digit(9))
        {
            continue;
        }
        if (index > 0 && std::isdigit(static_cast<unsigned char>(text[index - 1])) != 0)
        {
            continue;
        }
        const int year = std::stoi(text.substr(index, 4));
        const int month = std::stoi(text.substr(index + 5, 2));
        const int dayOfMonth = std::stoi(text.substr(index + 8, 2));
        if (year < 1970 || year > 2200 || month < 1 || month > 12 ||
            dayOfMonth < 1 || dayOfMonth > 31)
        {
            continue;
        }
        std::tm parts{};
        parts.tm_year = year - 1900;
        parts.tm_mon = month - 1;
        parts.tm_mday = dayOfMonth;
        const std::int64_t start = FromLocalParts(parts);
        if (start <= 0)
        {
            continue;
        }
        return {start, ShiftDays(start, 1), text.substr(index, 10)};
    }
    return {};
}
}

std::int64_t CurrentEpoch()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t ParseEpochSecondsText(const std::string& value)
{
    if (value.empty() || value.size() > 18 ||
        !std::all_of(value.begin(), value.end(),
            [](const unsigned char character) { return std::isdigit(character) != 0; }))
    {
        return 0;
    }
    return std::stoll(value);
}

TimeWindow ParseTimeWindow(const std::string& text, const std::int64_t nowEpoch)
{
    if (text.empty() || nowEpoch <= 0)
    {
        return {};
    }

    if (TimeWindow explicitDate = ParseExplicitDate(text); explicitDate.IsValid())
    {
        return explicitDate;
    }

    const std::vector<std::string> tokens = Tokenize(text);
    const std::string padded = Join(tokens);
    const std::int64_t today = StartOfDay(nowEpoch);

    // Ordered most specific first: "the day before yesterday" has to be tested before
    // "yesterday", and "last night" before "yesterday" would otherwise swallow it.
    if (Contains(padded, "day before yesterday"))
    {
        const std::int64_t start = ShiftDays(today, -2);
        return {start, ShiftDays(start, 1), "the day before yesterday"};
    }
    if (Contains(padded, "last night"))
    {
        return {TimeOfDay(ShiftDays(today, -1), 18), TimeOfDay(today, 4), "last night"};
    }
    if (Contains(padded, "yesterday morning"))
    {
        const std::int64_t start = ShiftDays(today, -1);
        return {TimeOfDay(start, 5), TimeOfDay(start, 12), "yesterday morning"};
    }
    if (Contains(padded, "this morning"))
    {
        return {TimeOfDay(today, 5), TimeOfDay(today, 12), "this morning"};
    }
    if (Contains(padded, "this afternoon"))
    {
        return {TimeOfDay(today, 12), TimeOfDay(today, 17), "this afternoon"};
    }
    if (Contains(padded, "this evening") || Contains(padded, "tonight"))
    {
        return {TimeOfDay(today, 17), ShiftDays(today, 1), "this evening"};
    }
    if (Contains(padded, "yesterday"))
    {
        return {ShiftDays(today, -1), today, "yesterday"};
    }
    if (Contains(padded, "the other day"))
    {
        return {ShiftDays(today, -4), today, "the other day"};
    }
    if (Contains(padded, "just now") || Contains(padded, "a moment ago") ||
        Contains(padded, "a second ago"))
    {
        return {nowEpoch - 15 * Minute, nowEpoch + Minute, "just now"};
    }
    if (Contains(padded, "earlier today") || Contains(padded, "so far today") ||
        Contains(padded, "today"))
    {
        return {today, nowEpoch + Minute, "today"};
    }

    // "<count> <unit> ago", anchored on the word "ago" and read backwards so that
    // "3 days ago" and "a couple of days ago" take the same path.
    for (std::size_t index = 2; index < tokens.size(); ++index)
    {
        if (tokens[index] != "ago")
        {
            continue;
        }
        const Unit unit = ParseUnit(tokens[index - 1]);
        if (unit == Unit::None)
        {
            continue;
        }
        std::string countToken = tokens[index - 2];
        int count = ParseCount(countToken);
        if (count < 0 && index >= 3 && countToken == "of")
        {
            countToken = tokens[index - 3];
            count = ParseCount(countToken);
        }
        if (count < 0)
        {
            continue;
        }
        const bool approximate = IsApproximateCount(countToken);
        const std::string phrase =
            std::to_string(count) + " " + tokens[index - 1] + " ago";
        switch (unit)
        {
            case Unit::Minutes:
            {
                const std::int64_t point = nowEpoch - count * Minute;
                return {point - 5 * Minute, point + 5 * Minute, phrase};
            }
            case Unit::Hours:
            {
                const std::int64_t point = nowEpoch - count * Hour;
                return {point - 30 * Minute, point + 30 * Minute, phrase};
            }
            case Unit::Days:
            {
                // A whole day, because "three days ago" names a day and not a moment.
                // A vague count covers the days either side of it instead.
                if (approximate)
                {
                    return {ShiftDays(today, -(count + 2)),
                            ShiftDays(today, -(count - 1)),
                            "a " + countToken + " days ago"};
                }
                const std::int64_t start = ShiftDays(today, -count);
                return {start, ShiftDays(start, 1), phrase};
            }
            case Unit::Weeks:
            {
                const std::int64_t start = StartOfWeek(ShiftDays(today, -7 * count));
                return {start, ShiftDays(start, 7), phrase};
            }
            case Unit::Months:
            {
                const std::int64_t start = StartOfMonth(ShiftMonths(today, -count));
                return {start, StartOfMonth(ShiftMonths(start, 1)), phrase};
            }
            case Unit::Years:
            {
                const std::int64_t start = StartOfDay(ShiftMonths(today, -12 * count));
                return {start, ShiftDays(start, 1), phrase};
            }
            case Unit::None:
                break;
        }
    }

    if (Contains(padded, "last week"))
    {
        const std::int64_t start = StartOfWeek(ShiftDays(StartOfWeek(nowEpoch), -1));
        return {start, ShiftDays(start, 7), "last week"};
    }
    if (Contains(padded, "this week"))
    {
        return {StartOfWeek(nowEpoch), nowEpoch + Minute, "this week"};
    }
    if (Contains(padded, "last month"))
    {
        const std::int64_t start = StartOfMonth(ShiftMonths(StartOfMonth(nowEpoch), -1));
        return {start, StartOfMonth(nowEpoch), "last month"};
    }
    if (Contains(padded, "this month"))
    {
        return {StartOfMonth(nowEpoch), nowEpoch + Minute, "this month"};
    }
    if (Contains(padded, "last year"))
    {
        const std::int64_t start = ShiftMonths(StartOfMonth(nowEpoch), -12);
        return {start, StartOfMonth(nowEpoch), "last year"};
    }

    // A bare or "last" weekday means the most recent one that has already happened. If
    // today is Tuesday, "last Tuesday" is a week ago rather than this morning.
    for (const std::string& token : tokens)
    {
        const int weekday = ParseWeekday(token);
        if (weekday < 0)
        {
            continue;
        }
        int back = LocalParts(nowEpoch).tm_wday - weekday;
        if (back <= 0)
        {
            back += 7;
        }
        const std::int64_t start = ShiftDays(today, -back);
        return {start, ShiftDays(start, 1), token};
    }

    return {};
}

std::string DescribeMoment(const std::int64_t epochSeconds, const std::int64_t nowEpoch)
{
    if (epochSeconds <= 0 || nowEpoch <= 0)
    {
        return "";
    }
    const std::tm parts = LocalParts(epochSeconds);
    std::ostringstream clock;
    clock << std::put_time(&parts, "%H:%M");

    const std::int64_t today = StartOfDay(nowEpoch);
    const std::int64_t moment = StartOfDay(epochSeconds);
    if (moment == today)
    {
        return nowEpoch - epochSeconds < 30 * Minute
            ? std::string("a few minutes ago")
            : "today " + clock.str();
    }
    if (moment == ShiftDays(today, -1))
    {
        return "yesterday " + clock.str();
    }
    if (moment > ShiftDays(today, -7) && moment < today)
    {
        std::ostringstream weekday;
        weekday << std::put_time(&parts, "%A ") << clock.str();
        return weekday.str();
    }

    std::ostringstream stamp;
    const bool sameYear = parts.tm_year == LocalParts(nowEpoch).tm_year;
    stamp << std::put_time(&parts, sameYear ? "%d %b %H:%M" : "%Y-%m-%d");
    return stamp.str();
}

std::string DescribeNow(const std::int64_t nowEpoch)
{
    if (nowEpoch <= 0)
    {
        return "";
    }
    const std::tm parts = LocalParts(nowEpoch);
    std::ostringstream stream;
    stream << std::put_time(&parts, "%A %d %B %Y, %H:%M");
    return stream.str();
}

} // namespace revia::memory
