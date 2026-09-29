#include "Speech/turnTaking.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace revia::speech
{

namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string Trim(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

// The last word, without the punctuation around it.
std::string LastWord(const std::string& lowered)
{
    std::string word;
    for (auto it = lowered.rbegin(); it != lowered.rend(); ++it)
    {
        const unsigned char character = static_cast<unsigned char>(*it);
        if (std::isalnum(character) || *it == '\'')
        {
            word.insert(word.begin(), *it);
        }
        else if (!word.empty())
        {
            break;
        }
    }
    return word;
}

std::size_t WordCount(const std::string& lowered)
{
    std::size_t words = 0;
    bool inWord = false;
    for (const char value : lowered)
    {
        const bool part = std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '\'';
        if (part && !inWord) ++words;
        inWord = part;
    }
    return words;
}

// Words nobody ends a thought on. A transcript that stops on one of these stopped
// because the person paused, not because they were done.
const std::vector<std::string>& DanglingWords()
{
    static const std::vector<std::string> words = {
        "and", "but", "or", "so", "because", "then", "with", "to", "the", "a", "an", "of",
        "if", "when", "that", "which", "who", "um", "uh", "er", "erm", "hmm", "like", "i",
        "i'm", "i've", "i'd", "you", "we", "they", "it's", "is", "are", "was", "were", "for",
        "in", "on", "at", "by", "from", "about", "into", "than", "as", "my", "your", "his",
        "her", "their", "our", "this", "these", "those", "what", "how", "why", "where",
        "can", "could", "would", "should", "will", "do", "does", "did", "not", "very"};
    return words;
}
} // namespace

std::string ToString(const TurnCompletion value)
{
    switch (value)
    {
        case TurnCompletion::Complete: return "complete";
        case TurnCompletion::Unfinished: return "unfinished";
        case TurnCompletion::Uncertain: return "uncertain";
    }
    return "uncertain";
}

TurnJudgement JudgeTurnCompletion(const std::string& transcript)
{
    TurnJudgement judgement;
    const std::string text = Trim(transcript);
    if (text.empty())
    {
        judgement.completion = TurnCompletion::Uncertain;
        judgement.because = "nothing was said";
        return judgement;
    }
    const std::string lowered = Lower(text);
    const char last = lowered.back();
    // A trailing comma, dash or ellipsis is the transcriber hearing the pause itself.
    if (last == ',' || last == '-' || lowered.ends_with("...") || lowered.ends_with("\xE2\x80\xA6"))
    {
        judgement.completion = TurnCompletion::Unfinished;
        judgement.because = "trailing pause mark";
        return judgement;
    }
    const std::string word = LastWord(lowered);
    if (std::find(DanglingWords().begin(), DanglingWords().end(), word) != DanglingWords().end())
    {
        judgement.completion = TurnCompletion::Unfinished;
        judgement.because = "ends on \"" + word + "\"";
        return judgement;
    }
    if (last == '.' || last == '!' || last == '?')
    {
        judgement.completion = TurnCompletion::Complete;
        judgement.because = "ends with a terminal mark";
        return judgement;
    }
    // A short whole phrase with nothing dangling ("okay", "sing something", "no") is
    // done even when the transcriber wrote no full stop.
    if (WordCount(lowered) <= 3)
    {
        judgement.completion = TurnCompletion::Complete;
        judgement.because = "a short whole phrase";
        return judgement;
    }
    judgement.completion = TurnCompletion::Uncertain;
    judgement.because = "no mark either way";
    return judgement;
}

int ContinuationWindowMs(const TurnCompletion completion, const int baseSilenceMs, const int extensionMs)
{
    const int base = std::max(0, baseSilenceMs);
    const int extension = std::max(0, extensionMs);
    switch (completion)
    {
        case TurnCompletion::Complete: return base;
        case TurnCompletion::Unfinished: return base + extension;
        case TurnCompletion::Uncertain: return base + extension / 2;
    }
    return base;
}

bool ThinkingFillerPolicy::AsksForThought(const std::string& input)
{
    const std::string lowered = Lower(Trim(input));
    if (lowered.empty()) return false;
    if (lowered.back() == '?') return true;
    static const std::vector<std::string> openers = {
        "explain", "why ", "how ", "what ", "which ", "compare", "work out", "figure out",
        "help me", "walk me through", "tell me why", "tell me how", "should i", "could you",
        "can you", "debug", "design"};
    return std::any_of(openers.begin(), openers.end(),
        [&lowered](const std::string& opener) { return lowered.rfind(opener, 0) == 0; });
}

bool ThinkingFillerPolicy::Consider(
    const std::string& input,
    const bool willThink,
    const bool speechAvailable,
    const std::uint64_t turnId,
    const Clock::time_point now)
{
    if (!willThink || !speechAvailable || turnId == lastTurn) return false;
    if (!AsksForThought(input)) return false;
    if (lastFiller != Clock::time_point{} && now - lastFiller < minimumInterval) return false;
    lastTurn = turnId;
    lastFiller = now;
    return true;
}

} // namespace revia::speech
