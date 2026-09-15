#include "Planning/operateIntent.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace revia::planning
{

namespace
{

std::string Normalize(const std::string& value)
{
    std::string lowered;
    lowered.reserve(value.size());
    bool pendingSpace = false;
    for (const unsigned char character : value)
    {
        if (std::isspace(character) != 0)
        {
            pendingSpace = !lowered.empty();
            continue;
        }
        // Punctuation that only ever ends a sentence is dropped so "open edge." and
        // "open edge" are the same request. Everything else is kept, because a dot
        // inside facebook.com is part of the target.
        if (character == '?' || character == '!' || character == ',')
        {
            continue;
        }
        if (pendingSpace) lowered.push_back(' ');
        pendingSpace = false;
        lowered.push_back(static_cast<char>(std::tolower(character)));
    }
    while (!lowered.empty() && lowered.back() == '.') lowered.pop_back();
    return lowered;
}

bool StartsWith(const std::string& value, const std::string_view prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

// Politeness and address, removed so the verb can be tested at the front. "Can you open
// Edge" is a request, not a question about her abilities, and speech produces far more
// of these than typing does.
bool StripLeadingCourtesy(std::string& value)
{
    static constexpr std::array<std::string_view, 16> prefixes = {
        "hey revia ", "revia ", "please ", "could you please ", "can you please ",
        "could you ", "can you ", "would you ", "will you ", "go ahead and ",
        "i want you to ", "i would like you to ", "i'd like you to ", "you can ",
        "now ", "just "};
    for (const std::string_view prefix : prefixes)
    {
        if (StartsWith(value, prefix))
        {
            value.erase(0, prefix.size());
            return true;
        }
    }
    return false;
}

// Verbs that name operating this machine. Multi-word forms come first so "go to" is not
// matched as the bare "go".
// "open up" is deliberately absent. As a phrasal verb it is almost always figurative --
// open up to me, open up about it -- and listing it consumed the "up" before the
// figurative check could see it, so "open up to me" arrived as a request to operate.
// Left to the bare "open", the remainder is "up to me" and the guard below catches it,
// while "open up edge" still matches on "open".
const std::array<std::string_view, 22>& OperateVerbs()
{
    static const std::array<std::string_view, 22> verbs = {
        "go to ", "navigate to ", "pull up ", "bring up ", "switch to ", "double-click ",
        "log in to ", "sign in to ",
        "open ", "launch ", "start ", "run ", "close ", "quit ", "click ", "type ",
        "press ", "scroll ", "minimize ", "maximize ", "focus ", "select "};
    return verbs;
}

// Figurative uses of the same verbs. Without these "open up to me" and "start a
// conversation" would both reach for the mouse, which is the failure that would make a
// natural-language route worse than no route at all.
bool ObjectIsFigurative(const std::string& remainder)
{
    static constexpr std::array<std::string_view, 18> figurative = {
        "up to me", "up to you", "up about", "up more", "your mind", "your heart",
        "yourself", "a conversation", "the conversation", "a discussion", "a dialogue",
        "a chat", "a debate", "an argument", "over", "again from the top", "fresh",
        "thinking"};
    for (const std::string_view phrase : figurative)
    {
        if (remainder == phrase || StartsWith(remainder, std::string(phrase) + " "))
        {
            return true;
        }
    }
    return false;
}

// A statement about what already happened is not an instruction. Checked after courtesy
// stripping so "i want you to open edge" survives and "i opened edge" does not.
bool IsReport(const std::string& value)
{
    static constexpr std::array<std::string_view, 8> reports = {
        "i ", "we ", "you already ", "you just ", "you were ", "you have ", "it ",
        "that "};
    for (const std::string_view prefix : reports)
    {
        if (StartsWith(value, prefix)) return true;
    }
    return false;
}

} // namespace

OperateIntent DetectOperateRequest(const std::string& input)
{
    OperateIntent intent;
    std::string value = Normalize(input);
    if (value.empty() || value.front() == '/') return intent;

    // Repeated because speech stacks them: "hey revia can you please open edge".
    for (int pass = 0; pass < 4 && StripLeadingCourtesy(value); ++pass) {}
    if (value.empty() || IsReport(value)) return intent;

    for (const std::string_view verb : OperateVerbs())
    {
        if (!StartsWith(value, verb)) continue;
        const std::string remainder = value.substr(verb.size());
        // A verb with nothing after it names no target, so there is nothing to do.
        if (remainder.empty() || ObjectIsFigurative(remainder)) return intent;
        intent.matched = true;
        intent.verb = std::string(verb.substr(0, verb.size() - 1));
        return intent;
    }
    return intent;
}

} // namespace revia::planning
