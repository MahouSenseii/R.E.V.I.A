#include "Agents/replyFragmenter.h"

#include <algorithm>
#include <cctype>

namespace revia::agents
{

namespace
{

bool IsTerminal(const char value)
{
    return value == '.' || value == '!' || value == '?';
}

bool EndsWithTerminalSentence(const std::string& text)
{
    std::size_t end = text.size();
    while (end > 0 &&
        (text[end - 1] == '"' || text[end - 1] == '\'' ||
            text[end - 1] == ')' || text[end - 1] == ']'))
    {
        --end;
    }
    return end > 0 && IsTerminal(text[end - 1]);
}

std::string Trim(const std::string& value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

// Abbreviations whose full stop is not the end of anything. Short list on purpose: a
// missed boundary only delays a fragment, while a wrong one cuts speech mid-clause.
bool EndsWithAbbreviation(const std::string& text, const std::size_t terminalIndex)
{
    static const char* abbreviations[] = {
        "mr", "mrs", "ms", "dr", "prof", "sr", "jr", "st", "vs", "etc", "e.g", "i.e",
        "fig", "approx", "no"
    };
    std::size_t start = terminalIndex;
    while (start > 0 && (std::isalpha(static_cast<unsigned char>(text[start - 1])) != 0 ||
        text[start - 1] == '.'))
    {
        --start;
    }
    std::string word = text.substr(start, terminalIndex - start);
    std::transform(word.begin(), word.end(), word.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    for (const char* abbreviation : abbreviations)
    {
        if (word == abbreviation)
        {
            return true;
        }
    }
    return false;
}

} // namespace

ReplyFragmenter::ReplyFragmenter(
    const std::size_t minimumFragmentCharacters,
    const std::size_t maximumPhraseCharacters,
    const std::size_t firstMinimumFragmentCharacters,
    const std::size_t firstMaximumPhraseCharacters)
    : followingMinimumCharacters(minimumFragmentCharacters),
      followingMaximumCharacters(maximumPhraseCharacters),
      firstMinimumCharacters(firstMinimumFragmentCharacters == 0
          ? minimumFragmentCharacters : firstMinimumFragmentCharacters),
      firstMaximumCharacters(firstMaximumPhraseCharacters == 0
          ? maximumPhraseCharacters : firstMaximumPhraseCharacters)
{
}

bool ReplyFragmenter::IsBoundary(const std::string& text, const std::size_t index)
{
    if (index >= text.size() || !IsTerminal(text[index]))
    {
        return false;
    }
    // A decimal point, a version number, an IP address: digits either side is never a
    // sentence end.
    if (text[index] == '.' && index > 0 && index + 1 < text.size() &&
        std::isdigit(static_cast<unsigned char>(text[index - 1])) != 0 &&
        std::isdigit(static_cast<unsigned char>(text[index + 1])) != 0)
    {
        return false;
    }
    if (text[index] == '.' && EndsWithAbbreviation(text, index))
    {
        return false;
    }

    // Run past a closing quote or bracket so the punctuation stays with its sentence, and
    // past an ellipsis so it is treated as one boundary rather than three.
    std::size_t after = index;
    while (after + 1 < text.size() && IsTerminal(text[after + 1]))
    {
        ++after;
    }
    while (after + 1 < text.size() &&
        (text[after + 1] == '"' || text[after + 1] == '\'' ||
            text[after + 1] == ')' || text[after + 1] == ']'))
    {
        ++after;
    }
    // The boundary is only real once whitespace confirms it. Mid-stream, a terminal at the
    // very end may still be an abbreviation whose next character has not arrived.
    return after + 1 < text.size() &&
        std::isspace(static_cast<unsigned char>(text[after + 1])) != 0;
}

std::vector<std::string> ReplyFragmenter::Consume(const std::string& incoming)
{
    pending += incoming;
    std::vector<std::string> fragments;

    std::size_t searchFrom = 0;
    while (searchFrom < pending.size())
    {
        const bool first = emittedFragments == 0;
        const std::size_t minimumCharacters = first
            ? firstMinimumCharacters : followingMinimumCharacters;
        const std::size_t maximumCharacters = first
            ? firstMaximumCharacters : followingMaximumCharacters;
        std::size_t boundary = std::string::npos;
        for (std::size_t index = searchFrom; index < pending.size(); ++index)
        {
            if (IsBoundary(pending, index))
            {
                std::size_t end = index;
                while (end + 1 < pending.size() &&
                    (IsTerminal(pending[end + 1]) || pending[end + 1] == '"' ||
                        pending[end + 1] == '\'' || pending[end + 1] == ')' ||
                        pending[end + 1] == ']'))
                {
                    ++end;
                }
                boundary = end;
                break;
            }
        }
        // Legacy character targets are retained for configuration compatibility, but
        // they are never permission to cut speech in the middle of a sentence. A long
        // sentence stays intact and is handed to one worker only after its terminal
        // punctuation is known.
        (void)maximumCharacters;
        if (boundary == std::string::npos)
        {
            break;
        }

        const std::string candidate = Trim(pending.substr(0, boundary + 1));
        // Too short to be worth interrupting the stream for. Kept with the next sentence
        // instead, so "Sure." does not become its own utterance.
        if (candidate.size() < minimumCharacters)
        {
            searchFrom = boundary + 1;
            continue;
        }
        fragments.push_back(candidate);
        ++emittedFragments;
        pending.erase(0, boundary + 1);
        searchFrom = 0;
    }
    return fragments;
}

std::string ReplyFragmenter::Flush()
{
    std::string remainder = Trim(pending);
    pending.clear();
    if (remainder.empty()) return remainder;
    if (EndsWithTerminalSentence(remainder)) return remainder;

    // If earlier complete sentences are already queued, a non-terminal tail normally
    // means the model hit a stop/token boundary mid-thought. Do not make Revia speak that
    // broken tail. A short one-line answer such as "Okay" is still a complete utterance;
    // give it natural terminal punctuation before TTS.
    if (emittedFragments > 0 || remainder.size() > 160) return {};
    remainder.push_back('.');
    return remainder;
}

void ReplyFragmenter::Reset()
{
    pending.clear();
    emittedFragments = 0;
}

} // namespace revia::agents
