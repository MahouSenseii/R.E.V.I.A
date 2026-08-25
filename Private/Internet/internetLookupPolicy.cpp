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

    const bool question = lowered.find('?') != std::string::npos ||
        lowered.starts_with("who ") || lowered.starts_with("what is ") ||
        lowered.starts_with("what are ") || lowered.starts_with("when ") ||
        lowered.starts_with("where ") || lowered.starts_with("how does ") ||
        lowered.starts_with("tell me about ");
    return question && lowered.size() >= 12;
}

} // namespace revia::internet
