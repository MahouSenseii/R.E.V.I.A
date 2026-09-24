#include "Agents/responseProvenance.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace revia::agents
{

namespace
{

std::string Lowered(const std::string& value)
{
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return lowered;
}

bool ContainsAny(
    const std::string& lowered,
    const std::initializer_list<std::string_view> markers)
{
    return std::any_of(markers.begin(), markers.end(),
        [&lowered](const std::string_view marker)
        {
            return lowered.find(marker) != std::string::npos;
        });
}

} // namespace

std::string ToString(const ResponseProvenance value)
{
    switch (value)
    {
        case ResponseProvenance::RequestedRepetition: return "requested_repetition";
        case ResponseProvenance::Roleplay: return "roleplay";
        case ResponseProvenance::RuntimeReflex: return "runtime_reflex";
        case ResponseProvenance::NormalGeneration:
        default: return "normal_generation";
    }
}

bool MayExpressOwnOpinion(const ResponseProvenance value)
{
    return value == ResponseProvenance::NormalGeneration;
}

ResponseProvenance ClassifyRequestedProvenance(const std::string& userInput)
{
    const std::string lowered = Lowered(userInput);

    // Being told to say particular words. Checked first: "pretend to be a critic and
    // repeat this line" is both, and repetition is the stricter reading of what the
    // reply will contain.
    if (ContainsAny(lowered, {
            "repeat exactly", "repeat after me", "repeat this", "repeat the following",
            "repeat back", "say exactly", "say this", "say the following",
            "say the words", "echo this", "echo back", "read this back",
            "read it back", "type exactly", "word for word", "verbatim"}))
    {
        return ResponseProvenance::RequestedRepetition;
    }

    // Being told to speak as someone else. "in character" and "as if you were" are
    // included because they are how the instruction is usually phrased mid-scene,
    // after "roleplay" was said once several turns earlier.
    if (ContainsAny(lowered, {
            "roleplay", "role-play", "role play", "pretend you", "pretend to be",
            "pretend that you", "act as if", "act as a", "act as the", "act like you",
            "in character as", "stay in character", "play the character",
            "play the role", "as if you were", "imagine you are", "you are now a",
            "you are now the", "speak as"}))
    {
        return ResponseProvenance::Roleplay;
    }

    return ResponseProvenance::NormalGeneration;
}

} // namespace revia::agents
