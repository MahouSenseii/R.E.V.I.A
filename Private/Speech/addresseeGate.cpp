#include "Speech/addresseeGate.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace revia::speech
{

AddresseeGate::AddresseeGate(AddresseeSettings initial) : settings(std::move(initial))
{
}

void AddresseeGate::Configure(AddresseeSettings updated)
{
    std::lock_guard lock(mutex);
    settings = std::move(updated);
    lastExchange.reset();
    followUpParticipant.clear();
    followUpAudience.clear();
    followUpRevision = 0;
}

bool AddresseeGate::MentionsWakeWord(const std::string& transcript, const std::vector<std::string>& wakeWords)
{
    std::string word;
    const auto matches = [&wakeWords](const std::string& candidate)
    {
        return !candidate.empty() &&
               std::any_of(wakeWords.begin(), wakeWords.end(),
                   [&candidate](const std::string& wake)
                   {
                       if (wake.size() != candidate.size())
                           return false;
                       return std::equal(wake.begin(), wake.end(), candidate.begin(),
                           [](const unsigned char left, const unsigned char right) { return std::tolower(left) == right; });
                   });
    };
    for (const unsigned char character : transcript)
    {
        if (std::isalpha(character) != 0)
        {
            word.push_back(static_cast<char>(std::tolower(character)));
            continue;
        }
        if (matches(word))
            return true;
        word.clear();
    }
    return matches(word);
}

bool AddresseeGate::Accept(const std::string& transcript, const Clock::time_point now, const bool inCall)
{
    return Accept(transcript, now, inCall, "legacy-participant", "legacy-audience", 0, true);
}

bool AddresseeGate::Accept(const std::string& transcript, const Clock::time_point now, const bool inCall, const std::string& participantId,
    const std::string& audienceId, const std::uint64_t audienceRevision, const bool foreground)
{
    std::lock_guard lock(mutex);
    if (!foreground)
        return false;
    const bool named = MentionsWakeWord(transcript, settings.wakeWords);
    const bool followUp = !inCall && lastExchange.has_value() && !participantId.empty() && followUpParticipant == participantId &&
                          followUpAudience == audienceId && followUpRevision == audienceRevision && now >= *lastExchange &&
                          now - *lastExchange <= std::chrono::seconds(settings.followUpSeconds);
    if (settings.requireWakeWord && !named && !followUp)
    {
        return false;
    }
    lastExchange = now;
    followUpParticipant = participantId;
    followUpAudience = audienceId;
    followUpRevision = audienceRevision;
    return true;
}

void AddresseeGate::NoteExchange(const Clock::time_point now)
{
    NoteExchange(now, "legacy-participant", "legacy-audience", 0);
}

void AddresseeGate::NoteExchange(
    const Clock::time_point now, const std::string& participantId, const std::string& audienceId, const std::uint64_t audienceRevision)
{
    std::lock_guard lock(mutex);
    lastExchange = now;
    followUpParticipant = participantId;
    followUpAudience = audienceId;
    followUpRevision = audienceRevision;
}

void AddresseeGate::Reset()
{
    std::lock_guard lock(mutex);
    lastExchange.reset();
    followUpParticipant.clear();
    followUpAudience.clear();
    followUpRevision = 0;
}

} // namespace revia::speech
