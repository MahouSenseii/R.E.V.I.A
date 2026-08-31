#include "Internet/internetLookupPolicy.h"

#include <algorithm>
#include <cctype>
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
}

bool InternetLookupPolicy::ShouldLookup(
    const std::string& input,
    const bool automaticLookup)
{
    const std::string lowered = Lower(input);
    if (lowered.empty() || lowered.size() > 1024)
    {
        return false;
    }
    if (ContainsAny(lowered, {
            "search the web", "search online", "look this up", "look it up",
            "look up ", "find online", "browse for ",
            "check online", "use the internet", "browse the web", "web search"}))
    {
        return true;
    }
    if (!automaticLookup)
    {
        return false;
    }
    // These are questions about Revia's retained local visual context, not requests for
    // fresh public facts. Words such as "current" and "right now" must not turn the
    // user's desktop into a DuckDuckGo query or add a browser round trip to the answer.
    if (ContainsAny(lowered, {
            "my screen", "my screens", "on screen", "on my monitor",
            "on my monitors", "what am i doing", "what i am doing",
            "what i'm doing", "what do you see", "can you see"}))
    {
        return false;
    }
    // Technical and programming questions are exactly what the local model is for, and
    // they collide badly with the time-sensitive words below: "what version of C++
    // supports this" would otherwise leave the machine because it contains "version".
    // Checked first so the exclusion wins.
    if (ContainsAny(lowered, {
            "c++", "python", "javascript", "typescript", "rust", "golang", "java ",
            "cmake", "compile", "compiler", "syntax", "function", "variable",
            "pointer", "template", "std::", "regex", "algorithm", "recursion",
            "in c#", "sql query", "stack trace", "segfault", "null pointer"}))
    {
        return false;
    }

    if (ContainsAny(lowered, {
            "latest", "today", "current ", "currently", "right now", "recent",
            "news", "weather", "forecast", "price", "release date", "version",
            "schedule", "score", "president", "ceo", "law", "regulation"}))
    {
        return true;
    }

    // Ordinary personal and social turns never leave the machine merely because they
    // contain a question mark.
    if (ContainsAny(lowered, {
            "how are you", "what do you think", "do you remember", "my name",
            "i feel", "i am ", "i'm ", "we were", "what are we working on",
            "do you ", "did you ", "are you ", "have you ", "would you ",
            "could you ", "your ", " you ", "who did you"}))
    {
        return false;
    }

    // A question mark is not evidence that the answer is on the internet.
    //
    // This used to return true for any input containing '?' that was at least twelve
    // characters long, which meant nearly every question the user asked paid for a web
    // round trip. Measured on an ordinary C++ question that cost 14.8 seconds out of an
    // 18.3 second turn -- the lookup was four times more expensive than generating the
    // answer, for a question the local model could answer on its own.
    //
    // A lookup now needs actual evidence that fresh external facts are wanted: either an
    // explicit request, handled above and always honoured, or a time-sensitive marker.
    // Everything else stays local, which is faster, more private, and correct far more
    // often than the alternative.
    return false;
}

} // namespace revia::internet
