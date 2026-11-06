#include "Agents/delegation.h"

#include "Core/speechAttribution.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace revia::agents
{
namespace
{
std::string Normalized(const std::string& input)
{
    std::string lowered;
    lowered.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        // U+2019, the apostrophe most keyboards and phones type, read as '.
        if (index + 2 < input.size() && static_cast<unsigned char>(input[index]) == 0xE2 &&
            static_cast<unsigned char>(input[index + 1]) == 0x80 && static_cast<unsigned char>(input[index + 2]) == 0x99)
        {
            lowered += '\'';
            index += 2;
            continue;
        }
        lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(input[index])));
    }
    return lowered;
}

// Where the earliest of these phrases starts, or npos.
std::size_t FirstOf(const std::string& text, const std::initializer_list<const char*> phrases)
{
    std::size_t first = std::string::npos;
    for (const char* phrase : phrases)
        first = std::min(first, text.find(phrase));
    return first;
}
}

std::string ToString(const DelegationMode mode)
{
    switch (mode)
    {
    case DelegationMode::Off:
        return "off";
    case DelegationMode::OnRequest:
        return "on_request";
    case DelegationMode::Auto:
        return "auto";
    }
    return "off";
}

DelegationMode DelegationModeFromString(const std::string& value)
{
    if (value == "on_request")
        return DelegationMode::OnRequest;
    if (value == "auto")
        return DelegationMode::Auto;
    return DelegationMode::Off;
}

DelegationCue ReadDelegationCue(const std::string& input)
{
    if (input.empty() || input.size() > 4000)
        return DelegationCue::None;
    // Only the person's own words: a quoted "don't use your agents" inside "use your agents to
    // check whether ..." is the subject of the question, not an instruction. Read whole, the
    // quote won, delegate_task was never offered, and a live request went undelegated.
    const std::string text = Normalized(conversation::ReadSpeechAttribution(input).userAuthoredText);
    // The instruction stated first decides. Checked as "any refusal anywhere wins", a live
    // "use your agents to check ... whether X counts as asking you to answer directly" was
    // read as a refusal. A refusal starting where a request does ("don't use your agents"
    // contains "use your agents") starts earlier, so it still wins.
    const std::size_t refusal = FirstOf(text, {"answer this directly", "answer that directly", "answer it directly", "answer directly", "just answer",
                              "don't delegate", "do not delegate", "dont delegate", "without your agents", "without the agents",
                              "without agents", "don't use your agents", "do not use your agents", "dont use your agents",
                              "don't use the agents", "do not use the agents", "don't use agents", "do not use agents", "no agents",
                              "don't use your workers", "do not use your workers", "without your workers"});
    // Phrases that name her workers or the act of handing work to them. Deliberately not
    // "in the background" on its own, which is as often about a picture or a sound.
    const std::size_t request = FirstOf(text, {"use your agents", "use the agents", "use agents", "use your workers", "use the workers", "use your agent",
                              "with your agents", "with your workers", "have your agents", "have your workers", "have the agents",
                              "get your agents", "send your agents", "ask your agents", "ask your workers", "let your agents",
                              "let your workers", "your agents to ", "your workers to ", "agent studio", "delegate this",
                              "delegate it", "delegate that", "delegate the ", "delegate to ", "hand this off", "hand it off",
                              "hand that off", "work on it in the background", "work on this in the background",
                              "look into it in the background", "investigate it in the background",
                              "investigate this in the background"});
    if (refusal == std::string::npos && request == std::string::npos)
        return DelegationCue::None;
    return refusal <= request ? DelegationCue::AnswerDirectly : DelegationCue::UseAgents;
}

bool OffersDelegation(const DelegationMode mode, const DelegationCue cue)
{
    if (mode == DelegationMode::Off || cue == DelegationCue::AnswerDirectly)
        return false;
    if (mode == DelegationMode::OnRequest)
        return cue == DelegationCue::UseAgents;
    return true;
}

} // namespace revia::agents
