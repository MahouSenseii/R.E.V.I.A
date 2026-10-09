#include "Internet/internetLookupPolicy.h"
#include "Core/speechAttribution.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string_view>

namespace revia::internet
{
namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ContainsAny(const std::string& input, const std::initializer_list<std::string_view> values)
{
    return std::any_of(values.begin(), values.end(), [&](const std::string_view value)
    {
        return input.find(value) != std::string::npos;
    });
}

bool ContainsWord(const std::string& input, const std::string_view word)
{
    const auto wordPart = [](const unsigned char c) { return std::isalnum(c) || c == '_'; };
    for (auto at = input.find(word); at != std::string::npos; at = input.find(word, at + 1))
    {
        const auto end = at + word.size();
        if ((at == 0 || !wordPart(input[at - 1])) && (end == input.size() || !wordPart(input[end])))
            return true;
    }
    return false;
}

bool ContainsAnyWord(const std::string& input, const std::initializer_list<std::string_view> words)
{
    return std::any_of(words.begin(), words.end(), [&](const auto word) { return ContainsWord(input, word); });
}

bool IsLocal(const std::string& input)
{
    return ContainsAnyWord(input, {"my", "our", "your"}) ||
           ContainsAny(input, {"my screen", "my screens", "on screen", "on my monitor", "what am i doing", "what i am doing",
                                  "what i'm doing", "what do you see", "can you see", "how are you", "what do you think", "do you remember",
                                  "my name", "i feel", "i am ", "i'm ", "we were", "what are we working on", "my schedule", "your schedule",
                                  "i paid", "my price", "my budget", "my appointment", "my calendar", "my news", "my score", "your price"});
}

bool HasFreshnessIntent(const std::string& input)
{
    if (ContainsAny(input,
            {"released yet", "release yet", "releases yet", "out yet", "available yet", "shipped yet", "launched yet", "come out yet"}))
        return true;
    const bool freshness = ContainsAnyWord(input, {"latest", "newest", "current", "currently", "recent", "up-to-date", "today"});
    if (freshness && ContainsAnyWord(input, {"version", "versions", "release", "releases"}))
        return true;
    const bool publicTopic = ContainsAnyWord(input, {"news", "weather", "forecast", "price", "prices", "schedule", "score", "president",
                                                        "ceo", "law", "regulation"}) ||
                             input.find("release date") != std::string::npos;
    const bool technical =
        ContainsAny(input, {"c++", "python", "javascript", "typescript", "rust", "golang", "java ", "cmake", "compile", "compiler",
                               "syntax", "function", "variable", "pointer", "template", "std::", "regex", "algorithm", "recursion", "in c#",
                               "sql query", "stack trace", "segfault", "null pointer"});
    return publicTopic && (freshness || !technical);
}

std::string IntentText(const std::string& input)
{
    auto masked = conversation::ReadSpeechAttribution(input).userAuthoredText;
    for (std::size_t at = 0; at < masked.size(); ++at)
    {
        if (masked[at] != '`')
            continue;
        const auto after = masked.find_first_not_of('`', at);
        const auto delimiter = masked.substr(at, after == std::string::npos ? masked.size() - at : after - at);
        const auto close = after == std::string::npos ? std::string::npos : masked.find(delimiter, after);
        const auto end = close == std::string::npos ? masked.size() : close + delimiter.size();
        std::fill(masked.begin() + at, masked.begin() + end, ' ');
        at = end - 1;
    }
    return Lower(std::move(masked));
}

std::string Compact(const std::string& input)
{
    std::string result;
    for (const unsigned char c : input)
    {
        if (!std::isspace(c))
            result += static_cast<char>(c);
        else if (!result.empty() && result.back() != ' ')
            result += ' ';
    }
    if (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result;
}
}

LookupRequest SelectLookupRequest(const std::string& input)
{
    LookupRequest selected;
    const auto masked = IntentText(input);
    static const std::regex explicitRequest(
        R"(^(?:(?:please|actually|instead|and|but|now|then|revia|hey revia|okay revia|can you|could you|would you|will you|i want you to)\s+)*(?:(do not|don't|never)(?:(?:,\s*|\s+)(?:ever|under any circumstances))*[ ,]+)?(?:search the web|search online|search the internet|look this up|look it up|look up|find online|browse for|check online|use the internet|browse the web|browse online|web search)\b)");
    static const std::regex generalDenial(
        R"(^(?:(?:please|actually|instead|and|but|now|then)[ ,]+)*(?:do not|don't|never|no|avoid|stop)(?:(?:,\s*|\s+)(?:ever|under any circumstances))*[ ,]+(?:search|searching|browse|browsing|web search(?:es)?|internet lookup|(?:use|using) the (?:web|internet))\b)");
    static const std::regex continuedNegation(R"(\b(?:do not|don't|never)(?:(?:,\s*|\s+)(?:ever|under any circumstances))*\s*$)");
    for (std::size_t begin = 0; begin < masked.size();)
    {
        auto end = begin;
        while (end < masked.size())
        {
            const char c = masked[end];
            const bool conjunction = c == ' ' &&
                (masked.compare(end + 1, 4, "and ") == 0 || masked.compare(end + 1, 4, "but ") == 0 ||
                    masked.compare(end + 1, 5, "then ") == 0);
            const bool nextDirective =
                (c == ',' || conjunction) &&
                (std::regex_search(Compact(masked.substr(end + 1, 512)), explicitRequest) ||
                    std::regex_search(Compact(masked.substr(end + 1, 512)), generalDenial)) &&
                !std::regex_search(Compact(masked.substr(begin, std::min<std::size_t>(end - begin, 512))), continuedNegation);
            if (c == '\n' || c == '\r' || c == ';' || nextDirective || c == '?' || c == '!' ||
                (c == '.' && (end + 1 == masked.size() || std::isspace(static_cast<unsigned char>(masked[end + 1])))))
                break;
            ++end;
        }
        const auto clause = Compact(masked.substr(begin, end - begin));
        std::smatch match;
        const auto prefix = clause.substr(0, 512);
        if (std::regex_search(prefix, generalDenial))
        {
            selected.prohibited = true;
            selected.explicitRequest = false;
            selected.text.clear();
        }
        else if (std::regex_search(prefix, match, explicitRequest))
        {
            selected.prohibited = match[1].matched;
            selected.explicitRequest = !selected.prohibited;
            selected.text = selected.prohibited ? std::string{} : input.substr(begin, end - begin);
        }
        else if (!selected.explicitRequest && !selected.prohibited && !IsLocal(clause) && HasFreshnessIntent(clause))
            selected.text = input.substr(begin, end - begin);
        begin = end < masked.size() ? end + 1 : end;
    }
    if (input.size() > 65536)
    {
        // Preserve authored intent for the limitation response, including later
        // withdrawals. Oversized requests never authorize an external lookup.
        selected.reason = "Lookup intent exceeds the 65,536-byte admission limit; please name a shorter public request.";
        selected.text.clear();
    }
    return selected;
}

bool InternetLookupPolicy::ShouldLookup(const std::string& input, const bool automaticLookup)
{
    const auto request = SelectLookupRequest(input);
    return !request.prohibited && request.reason.empty() && !request.text.empty() && (request.explicitRequest || automaticLookup);
}
} // namespace revia::internet
