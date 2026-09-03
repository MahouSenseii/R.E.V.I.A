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

// Whole-word match, because the words that signal freshness are also substrings of words
// that do not. "concurrent" contains "current", and a question about concurrent access
// is not a question about what shipped this week.
bool ContainsWord(const std::string& input, const std::string_view word)
{
    const auto isPartOfWord = [](const unsigned char character)
    {
        return std::isalnum(character) != 0 || character == '_';
    };
    for (std::size_t at = input.find(word); at != std::string::npos;
        at = input.find(word, at + 1))
    {
        const bool leftClear = at == 0 ||
            !isPartOfWord(static_cast<unsigned char>(input[at - 1]));
        const std::size_t after = at + word.size();
        const bool rightClear = after >= input.size() ||
            !isPartOfWord(static_cast<unsigned char>(input[after]));
        if (leftClear && rightClear) return true;
    }
    return false;
}

bool ContainsAnyWord(
    const std::string& input,
    const std::initializer_list<std::string_view> words)
{
    return std::any_of(words.begin(), words.end(), [&](const std::string_view word)
    {
        return ContainsWord(input, word);
    });
}

// "Did Python 3.14 release yet?" is a question about the state of the world even though
// it never says "latest". These phrases only appear when someone is asking whether
// something has happened yet, so they carry freshness on their own.
bool AsksReleaseStatus(const std::string& lowered)
{
    return ContainsAny(lowered, {
        "released yet", "release yet", "releases yet", "out yet", "available yet",
        "shipped yet", "launched yet", "come out yet"});
}

// Whether the turn is asking what is true in the world right now, rather than using an
// ordinary English adjective that happens to be temporal.
//
// The distinction that matters is not the adjective but what it is applied to. "What is
// the latest element in this vector?" and "What is the current value of this pointer?"
// both read as freshness if "latest" or "current" alone is enough, and neither has
// anything to do with the internet. So a freshness word has to land together with a word
// about a shipped thing -- a version or a release -- before this counts as wanting fresh
// external facts.
//
// Co-occurrence rather than adjacency, because the two orders are equally natural:
// "the latest Python version" and "what version of Qwen is current".
//
// The release-noun list is deliberately short. "build" and "update" were left out: "the
// current build system" and "the current update logic" are ordinary static questions,
// and admitting them would trade this fix for a new class of false positives.
bool HasFreshnessIntent(const std::string& lowered)
{
    if (AsksReleaseStatus(lowered)) return true;
    const bool freshnessWord = ContainsAnyWord(lowered, {
        "latest", "newest", "current", "currently", "recent", "up-to-date"});
    const bool releaseNoun = ContainsAnyWord(lowered, {
        "version", "versions", "release", "releases"});
    return freshnessWord && releaseNoun;
}

// Questions the local model is for. Stable knowledge does not change between the time
// the model was trained and the time the user asks, so a web round trip buys nothing and
// costs seconds.
bool IsStaticTechnicalKnowledge(const std::string& lowered)
{
    return ContainsAny(lowered, {
        "c++", "python", "javascript", "typescript", "rust", "golang", "java ",
        "cmake", "compile", "compiler", "syntax", "function", "variable",
        "pointer", "template", "std::", "regex", "algorithm", "recursion",
        "in c#", "sql query", "stack trace", "segfault", "null pointer"});
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
    // Asked before the technical exclusion below, and this ordering is the whole point.
    //
    // The exclusion used to win outright, so "What's the latest Python version?" stayed
    // local because it contains "python" -- the one kind of technical question the local
    // model genuinely cannot answer, since a model cannot know what shipped after it was
    // trained. A technical subject must not erase a clear question about current state.
    //
    // This is not the old check moved upward. The old freshness list treated a bare
    // "version" as time-sensitive, so hoisting it would have sent "Which C++ version
    // introduced std::format?" to the web. HasFreshnessIntent requires a freshness word
    // and a release word together, which is what separates "the latest Python version"
    // from "what Python version added match".
    if (HasFreshnessIntent(lowered))
    {
        return true;
    }
    // Technical and programming questions are exactly what the local model is for, and
    // they collide badly with the broad time-sensitive words below: "what version of C++
    // supports this" would otherwise leave the machine because it contains "version".
    if (IsStaticTechnicalKnowledge(lowered))
    {
        return false;
    }

    // What is left here is topic, not tense: subjects that change in the world on their
    // own, so naming one is already evidence that fresh facts are wanted.
    //
    // Three entries were removed, and they were the naive half of the old rule.
    //
    // Bare "version" made every historical question about when something shipped --
    // "What Unreal version introduced Lumen?" -- leave the machine. It only looked
    // harmless because the technical exclusion happened to catch the C++ and Python
    // phrasings first, and nothing caught Unreal.
    //
    // Bare "latest" and "current " were adjectives standing in for intent. "What is the
    // latest element in this vector?" and "What does current thread mean?" are ordinary
    // static questions, and "current " also matched inside "concurrent". An adjective is
    // a freshness signal only once it lands on something that actually ships, which is
    // what HasFreshnessIntent decides above; on its own it is just English.
    //
    // "release date" stays: asking for a date is asking about a schedule, not a language.
    if (ContainsAny(lowered, {
            "today", "currently", "right now",
            "news", "weather", "forecast", "price", "release date",
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
