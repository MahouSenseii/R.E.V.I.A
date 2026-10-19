#include "Agents/delegation.h"

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

bool ContainsAny(const std::string& text, const std::initializer_list<const char*> phrases)
{
    return std::any_of(phrases.begin(), phrases.end(), [&text](const char* phrase) { return text.find(phrase) != std::string::npos; });
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
    const std::string text = Normalized(input);
    // Asked first: "don't use your agents" contains "use your agents".
    if (ContainsAny(text, {"answer this directly", "answer that directly", "answer it directly", "answer directly", "just answer",
                              "don't delegate", "do not delegate", "dont delegate", "without your agents", "without the agents",
                              "without agents", "don't use your agents", "do not use your agents", "dont use your agents",
                              "don't use the agents", "do not use the agents", "don't use agents", "do not use agents", "no agents",
                              "don't use your workers", "do not use your workers", "without your workers"}))
        return DelegationCue::AnswerDirectly;
    // Phrases that name her workers or the act of handing work to them. Deliberately not
    // "in the background" on its own, which is as often about a picture or a sound.
    if (ContainsAny(text, {"use your agents", "use the agents", "use agents", "use your workers", "use the workers", "use your agent",
                              "with your agents", "with your workers", "have your agents", "have your workers", "have the agents",
                              "get your agents", "send your agents", "ask your agents", "ask your workers", "let your agents",
                              "let your workers", "your agents to ", "your workers to ", "agent studio", "delegate this",
                              "delegate it", "delegate that", "delegate the ", "delegate to ", "hand this off", "hand it off",
                              "hand that off", "work on it in the background", "work on this in the background",
                              "look into it in the background", "investigate it in the background",
                              "investigate this in the background"}))
        return DelegationCue::UseAgents;
    return DelegationCue::None;
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
