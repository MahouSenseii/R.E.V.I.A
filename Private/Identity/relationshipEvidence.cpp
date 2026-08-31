#include "Identity/relationshipEvidence.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

namespace revia::identity
{

namespace
{
    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    bool ContainsAny(
        const std::string& haystack,
        const std::initializer_list<std::string_view> needles)
    {
        return std::any_of(needles.begin(), needles.end(),
            [&haystack](const std::string_view needle)
            {
                return haystack.find(needle) != std::string::npos;
            });
    }
}

ConversationSignals ReadConversationSignals(
    const std::string& userInput,
    const std::string& reply,
    const bool succeeded)
{
    ConversationSignals signals;
    signals.userInput = userInput;
    signals.reply = reply;
    signals.succeeded = succeeded;

    const std::string input = Lower(userInput);

    // Aimed at Revia, not at the problem. The distinction matters: "this is broken" is
    // not evidence about the relationship, and treating it as such would make her resent
    // anyone doing difficult work with her.
    signals.hostileTowardRevia = ContainsAny(input, {
        "i hate you", "you're useless", "you are useless", "you're stupid",
        "you are stupid", "shut up", "nobody likes you", "you suck",
        "you're worthless", "you are worthless"});

    signals.expressedAppreciation = ContainsAny(input, {
        "thank", "thanks", "appreciate", "well done", "good job", "nice work",
        "that helped", "perfect", "exactly right"});

    signals.repeatedCorrection = ContainsAny(input, {
        "you keep", "i already said", "i just said", "again", "still not",
        "that's not what", "that is not what", "no, i meant"});

    signals.collaborative = succeeded && ContainsAny(input, {
        "let's", "lets ", "we should", "can you help", "work on", "together",
        "figure out", "let us"});

    // A short acknowledgement is not a relationship event. Weighting every "ok" the same
    // as a real exchange would let idle chatter accumulate into closeness.
    const bool substantial = userInput.size() > 24;
    signals.importance = substantial ? 0.45F : 0.2F;
    if (signals.hostileTowardRevia || signals.expressedAppreciation)
    {
        // Something was said about her specifically, which counts for more either way.
        signals.importance = 0.7F;
    }
    return signals;
}

std::string ReadStatedName(const std::string& userInput)
{
    static const std::vector<std::string> openers = {
        "my name is ", "call me ", "i am called ", "i'm called ", "you can call me ",
        "name's ", "this is "
    };
    const std::string lowered = Lower(userInput);
    for (const std::string& opener : openers)
    {
        const std::size_t at = lowered.find(opener);
        if (at == std::string::npos)
        {
            continue;
        }
        std::size_t index = at + opener.size();
        std::string name;
        // One word, letters and a couple of joining characters only. Taking the rest of
        // the sentence would store "quentin and i work on revia" as somebody's name.
        while (index < userInput.size() && name.size() < 32)
        {
            const unsigned char character = static_cast<unsigned char>(userInput[index]);
            if (std::isalpha(character) != 0 || character == '-' || character == '\'')
            {
                name.push_back(userInput[index]);
                ++index;
                continue;
            }
            break;
        }
        if (name.size() < 2)
        {
            continue;
        }
        // Rejecting these matters: "call me later" and "this is fine" both match an
        // opener, and storing them would rename the person to a stray word.
        static const std::vector<std::string> notNames = {
            "later", "back", "when", "if", "fine", "good", "it", "the", "a", "an",
            "me", "you", "your", "my", "and", "but", "so", "that", "this", "what"
        };
        const std::string loweredName = Lower(name);
        if (std::find(notNames.begin(), notNames.end(), loweredName) != notNames.end())
        {
            continue;
        }
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        return name;
    }
    return {};
}

RelationshipEvent BuildRelationshipEvent(
    const std::string& entityId,
    const ConversationSignals& signals)
{
    RelationshipEvent event;
    event.entityId = entityId;
    event.importance = std::clamp(signals.importance, 0.0F, 1.0F);
    // Read from keywords, not measured. The registry scales every delta by this, so an
    // inference the reader is unsure of barely moves anything.
    event.confidence = 0.55F;

    if (signals.expressedAppreciation)
    {
        event.positiveInteraction = 0.7F;
        // Being thanked is evidence that what she said held up, which is what trust is
        // actually made of.
        event.trustEvidence = 0.35F;
        event.description = "expressed appreciation";
    }
    if (signals.collaborative)
    {
        event.cooperation = std::max(event.cooperation, 0.6F);
        event.positiveInteraction = std::max(event.positiveInteraction, 0.4F);
        if (event.description.empty()) event.description = "worked on something together";
    }
    if (signals.hostileTowardRevia)
    {
        event.negativeInteraction = 0.8F;
        // Only hostility aimed at her counts as disrespect. This is the one signal that
        // accrues as lasting grievance rather than passing friction.
        event.disrespectEvidence = 0.5F;
        event.conflict = 0.5F;
        event.description = "a remark aimed at her";
    }
    else if (signals.repeatedCorrection)
    {
        // Friction, not disrespect. Being corrected repeatedly is her failing to listen,
        // and holding a grudge about being told so would be exactly backwards.
        event.conflict = 0.35F;
        event.negativeInteraction = 0.25F;
        if (event.description.empty()) event.description = "had to repeat themselves";
    }
    if (!signals.succeeded)
    {
        // The turn itself went wrong. That is not the user's fault and must not read as
        // conflict; it only dampens the positive side.
        event.positiveInteraction *= 0.5F;
        event.cooperation *= 0.5F;
    }
    if (event.description.empty())
    {
        event.description = "an ordinary exchange";
    }
    return event;
}

} // namespace revia::identity
