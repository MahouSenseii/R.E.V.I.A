#include "Speech/addresseeGate.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace revia::speech
{

AddresseeGate::AddresseeGate(AddresseeSettings initial)
    : settings(std::move(initial))
{
}

void AddresseeGate::Configure(AddresseeSettings updated)
{
    std::lock_guard lock(mutex);
    settings = std::move(updated);
}

bool AddresseeGate::MentionsWakeWord(
    const std::string& transcript, const std::vector<std::string>& wakeWords)
{
    std::string word;
    const auto matches = [&wakeWords](const std::string& candidate)
    {
        return !candidate.empty() && std::any_of(wakeWords.begin(), wakeWords.end(),
            [&candidate](const std::string& wake)
            {
                if (wake.size() != candidate.size()) return false;
                return std::equal(wake.begin(), wake.end(), candidate.begin(),
                    [](const unsigned char left, const unsigned char right)
                    {
                        return std::tolower(left) == right;
                    });
            });
    };
    for (const unsigned char character : transcript)
    {
        if (std::isalpha(character) != 0)
        {
            word.push_back(static_cast<char>(std::tolower(character)));
            continue;
        }
        if (matches(word)) return true;
        word.clear();
    }
    return matches(word);
}

bool AddresseeGate::Accept(
    const std::string& transcript, const Clock::time_point now, const bool inCall)
{
    std::lock_guard lock(mutex);
    const bool named = MentionsWakeWord(transcript, settings.wakeWords);
    const bool followUp = !inCall && lastExchange.has_value() &&
        now >= *lastExchange &&
        now - *lastExchange <= std::chrono::seconds(settings.followUpSeconds);
    if (settings.requireWakeWord && !named && !followUp)
    {
        return false;
    }
    lastExchange = now;
    return true;
}

void AddresseeGate::NoteExchange(const Clock::time_point now)
{
    std::lock_guard lock(mutex);
    lastExchange = now;
}

} // namespace revia::speech
