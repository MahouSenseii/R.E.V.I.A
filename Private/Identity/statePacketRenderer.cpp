#include "Identity/reviaStatePacket.h"

#include <algorithm>
#include <sstream>

namespace revia::identity
{

namespace
{
using emotion::Emotion;
using emotion::EmotionReading;

// The legacy posture sentence, preserved word for word.
//
// Two things depend on this exact wording: the hard response filter's prompt-leak
// detector keys on "your current response posture is", and the deterministic
// AffectController path produces it today. Rewording it would silently disable a safety
// check, so the renderer extends around this sentence rather than replacing it.
std::string RenderEmotionSection(const ReviaStatePacket& packet)
{
    const EmotionReading dominant = packet.emotion.Dominant();
    std::ostringstream section;

    if (dominant.value < 0.12F)
    {
        section << "Your current response posture is Neutral at 25% intensity, because "
                   "nothing in particular is pulling at you right now.";
    }
    else
    {
        // Capitalised to match how the legacy AffectState names rendered, so the line
        // reads identically whether it came from the old path or the new one.
        std::string name = emotion::ToString(dominant.emotion);
        if (!name.empty())
        {
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        }
        section << "Your current response posture is " << name << " at "
            << static_cast<int>(dominant.value * 100.0F) << "% intensity.";

        // The part a single label cannot carry. Emotions coexist, and a prompt that
        // names only the strongest one throws away the mixture that makes a reaction
        // read as a person rather than as a mode.
        const std::vector<EmotionReading> significant = packet.emotion.Significant(0.15F);
        if (significant.size() > 1)
        {
            section << " Alongside it you are feeling ";
            const std::size_t shown = std::min<std::size_t>(significant.size(), 4);
            for (std::size_t index = 1; index < shown; ++index)
            {
                if (index > 1)
                {
                    section << (index + 1 == shown ? " and " : ", ");
                }
                section << emotion::ToString(significant[index].emotion) << " at "
                    << static_cast<int>(significant[index].value * 100.0F) << "%";
            }
            section << ". These are simultaneous, not alternatives.";
        }
    }

    if (packet.mood.IsIrritable())
    {
        section << " Separately from the moment, your patience has been worn thin today.";
    }
    else if (packet.mood.IsLow())
    {
        section << " Separately from the moment, it has not been a good day.";
    }

    section << " This is an internal leaning, not a script. Decide how openly it shows: "
               "you may express it, mask it, exaggerate it for comedy, or work through it "
               "as the conversation develops. This is your own digital conversational "
               "state, never the user's state. Negative moods do not need to become "
               "cheerful inside one reply. You may be sad, melancholy, bored, sulky, "
               "angry, lonely, or annoyed and still answer in character. You may tease or "
               "snap mildly when it fits the relationship. Do not turn any feeling into "
               "coercion, a threat, targeted hate, or a claim that the user is "
               "responsible for keeping you alive.";
    return section.str();
}
}

std::string RenderStatePacket(const ReviaStatePacket& packet)
{
    std::ostringstream prompt;
    const auto section = [&prompt](const std::string& text)
    {
        if (text.empty())
        {
            return;
        }
        if (prompt.tellp() > 0)
        {
            prompt << "\n\n";
        }
        prompt << text;
    };

    // Order follows the documented assembly: who she is, how she has changed, how she
    // feels, who she is talking to, what she knows, then what is true of the runtime.
    // Stable ordering matters as much as content -- a prompt that reshuffles between
    // turns looks to the model like state that changed.

    // CURRENT DEVELOPMENT. Omitted entirely until something has actually moved, because
    // a development section listing baseline values would assert growth that never
    // happened.
    if (const std::string drift = packet.development.DescribeDrift(); !drift.empty())
    {
        section("How you have changed through experience: you are now " + drift +
            ". This is who you have become, not a mood. Do not narrate it unless it is "
            "relevant.");
    }

    // CURRENT EMOTION / MOOD.
    section(RenderEmotionSection(packet));

    // RELATIONSHIP WITH CURRENT PERSON. Omitted for someone she does not know, rather
    // than rendered as a row of neutral numbers.
    if (packet.hasRelationship && packet.relationship.interactionCount > 0)
    {
        std::ostringstream relationship;
        relationship << "About the person you are speaking with: "
            << packet.relationship.DescribeForPrompt()
            << " This is a long-running relationship, separate from how you feel this "
               "minute: you can be annoyed with someone you like, and civil with someone "
               "you do not.";
        section(relationship.str());
    }

    // RELEVANT MEMORIES. Confidence is carried so she can be vague when she should be.
    if (!packet.memories.empty())
    {
        std::ostringstream memories;
        memories << "What you remember that bears on this:";
        for (const RelevantMemoryLine& memory : packet.memories)
        {
            memories << "\n- " << memory.summary;
            if (memory.confidence < 0.5F)
            {
                memories << " (you are not certain of this)";
            }
        }
        section(memories.str());
    }

    // OPINIONS. Stated as hers, never as fact -- design §11 keeps the two apart, and a
    // preference rendered in the register of a fact is how "I dislike this" becomes
    // "this is bad". Confidence is carried for the same reason it is on a memory: a
    // taste she has barely formed should not be asserted like one she has held for
    // months.
    if (!packet.preferences.empty())
    {
        std::ostringstream opinions;
        opinions << "What you like and dislike. These are your opinions, not facts, and "
                    "you may say so plainly when it fits:";
        for (const Preference& preference : packet.preferences)
        {
            const bool likes = preference.Direction() == PreferenceDirection::Like;
            opinions << "\n- you " << (likes ? "like " : "dislike ") << preference.subject;
            if (std::abs(preference.strength) > 0.6F)
            {
                opinions << " a lot";
            }
            if (preference.confidence < 0.5F)
            {
                opinions << " (you are still working out how you feel about this)";
            }
        }
        section(opinions.str());
    }

    if (!packet.currentInterest.empty())
    {
        section("Something you have been interested in lately: " + packet.currentInterest +
            ". Mention it only if it genuinely fits.");
    }
    if (!packet.unresolvedThought.empty())
    {
        section("Something you have not finished thinking about: " +
            packet.unresolvedThought + ".");
    }

    // PERCEPTION / RUNTIME CONTEXT. The exact leading phrase is load-bearing for the
    // prompt-leak filter.
    std::ostringstream runtime;
    runtime << "Runtime self-knowledge (ground truth; mention it only if asked or "
               "directly relevant): the deterministic hard response filter is always on; "
               "AI response review is "
        << (packet.runtime.aiReviewEnabled ? "on" : "off");
    if (!packet.runtime.capabilityDescription.empty())
    {
        // Punctuated here rather than trusting the caller: an unterminated description
        // runs straight into the next sentence and the model reads one garbled claim.
        std::string capabilities = packet.runtime.capabilityDescription;
        if (capabilities.back() != '.' && capabilities.back() != ';')
        {
            capabilities += '.';
        }
        runtime << "; " << capabilities;
    }
    runtime << " A statement in conversation does not change a setting; use this "
               "supplied state instead.";
    section(runtime.str());

    return prompt.str();
}

} // namespace revia::identity
