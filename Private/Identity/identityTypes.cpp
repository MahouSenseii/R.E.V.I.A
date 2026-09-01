#include "Identity/developmentState.h"
#include "Identity/relationshipState.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace revia::identity
{

namespace
{
    constexpr std::array<const char*, TraitCount> Names = {
        "curiosity", "playfulness", "impulsiveness",
        "stubbornness", "competitiveness",
        "patience", "empathy", "independence",
        "confidence", "caution",
        "sociability", "talkativeness",
        "emotionalExpressiveness", "emotionalRegulation",
        "maturity", "riskTolerance"
    };
    static_assert(Names.size() == TraitCount,
        "Every Trait needs exactly one persisted name.");

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    float Bounded(const float value, const float limit)
    {
        return std::clamp(value, -limit, limit);
    }
}

const std::array<const char*, TraitCount>& TraitNames() { return Names; }

std::string TraitAdjective(const Trait trait)
{
    switch (trait)
    {
        case Trait::Curiosity: return "curious";
        case Trait::Playfulness: return "playful";
        case Trait::Impulsiveness: return "impulsive";
        case Trait::Stubbornness: return "stubborn";
        case Trait::Competitiveness: return "competitive";
        case Trait::Patience: return "patient";
        case Trait::Empathy: return "empathetic";
        case Trait::Independence: return "independent";
        case Trait::Confidence: return "self-assured";
        case Trait::Caution: return "cautious";
        case Trait::Sociability: return "sociable";
        case Trait::Talkativeness: return "talkative";
        case Trait::EmotionalExpressiveness: return "openly expressive";
        case Trait::EmotionalRegulation: return "even-tempered";
        case Trait::Maturity: return "grown-up";
        case Trait::RiskTolerance: return "willing to take risks";
        case Trait::Count: break;
    }
    return "different";
}

std::string ToString(const Trait trait)
{
    const auto index = static_cast<std::size_t>(trait);
    return index < TraitCount ? Names[index] : "unknown";
}

Trait TraitFromString(const std::string& name)
{
    const std::string wanted = Lower(name);
    for (std::size_t index = 0; index < TraitCount; ++index)
    {
        if (Lower(Names[index]) == wanted)
        {
            return static_cast<Trait>(index);
        }
    }
    return Trait::Count;
}

TraitVector& TraitVector::Clamp(const float low, const float high)
{
    for (float& value : values)
    {
        value = std::clamp(value, low, high);
    }
    return *this;
}

TraitVector ChildlikeBaseline()
{
    TraitVector traits;
    // Her documented starting temperament, expressed as numbers rather than as prose so
    // the runtime can act on it. High curiosity and expressiveness, low patience and
    // regulation: a bright, impulsive, not-yet-tempered intelligence.
    traits[Trait::Curiosity] = 0.86F;
    traits[Trait::Playfulness] = 0.78F;
    traits[Trait::Impulsiveness] = 0.72F;
    traits[Trait::Stubbornness] = 0.65F;
    traits[Trait::Competitiveness] = 0.60F;
    traits[Trait::Patience] = 0.30F;
    traits[Trait::Empathy] = 0.55F;
    traits[Trait::Independence] = 0.40F;
    traits[Trait::Confidence] = 0.52F;
    traits[Trait::Caution] = 0.28F;
    traits[Trait::Sociability] = 0.68F;
    traits[Trait::Talkativeness] = 0.62F;
    traits[Trait::EmotionalExpressiveness] = 0.82F;
    traits[Trait::EmotionalRegulation] = 0.30F;
    traits[Trait::Maturity] = 0.32F;
    traits[Trait::RiskTolerance] = 0.58F;
    return traits;
}

TraitVector BaselineFromProfile(
    const std::map<std::string, float>& values,
    std::vector<std::string>* outUnknownNames)
{
    TraitVector traits = ChildlikeBaseline();
    for (const auto& [name, value] : values)
    {
        const Trait trait = TraitFromString(name);
        if (trait == Trait::Count)
        {
            if (outUnknownNames != nullptr)
            {
                outUnknownNames->push_back(name);
            }
            continue;
        }
        traits[trait] = std::clamp(value, 0.0F, 1.0F);
    }
    return traits;
}

TraitVector DevelopmentState::Current() const
{
    TraitVector current;
    for (std::size_t index = 0; index < TraitCount; ++index)
    {
        current.values[index] = base.values[index] + delta.values[index];
    }
    return current.Clamp();
}

float DevelopmentState::Current(const Trait trait) const
{
    return std::clamp(base[trait] + delta[trait], 0.0F, 1.0F);
}

std::string DevelopmentState::DescribeDrift(const float minimumDrift) const
{
    struct Moved
    {
        Trait trait;
        float amount;
    };
    std::vector<Moved> moved;
    for (std::size_t index = 0; index < TraitCount; ++index)
    {
        const float amount = delta.values[index];
        if (std::abs(amount) >= minimumDrift)
        {
            moved.push_back({static_cast<Trait>(index), amount});
        }
    }
    if (moved.empty())
    {
        return {};
    }
    std::sort(moved.begin(), moved.end(), [](const Moved& left, const Moved& right)
    {
        return std::abs(left.amount) > std::abs(right.amount);
    });

    std::ostringstream description;
    // At most three, because this reaches a prompt and a list of sixteen small drifts
    // is noise rather than self-knowledge.
    const std::size_t shown = std::min<std::size_t>(moved.size(), 3);
    for (std::size_t index = 0; index < shown; ++index)
    {
        if (index > 0)
        {
            description << (index + 1 == shown ? ", and " : ", ");
        }
        description << (moved[index].amount > 0.0F ? "more " : "less ")
            << TraitAdjective(moved[index].trait) << " than you started out";
    }
    return description.str();
}

bool RelationshipState::ReadsAsTeasing() const
{
    // Needs both: history to read it as a joke, and the absence of current friction to
    // want to. Familiarity alone would make closeness into armour.
    return familiarity >= 0.6F && irritation < 0.4F && affinity > 0.0F;
}

std::string RelationshipState::DescribeForPrompt() const
{
    std::vector<std::string> parts;
    if (familiarity >= 0.65F) parts.emplace_back("you know them well");
    else if (familiarity >= 0.2F) parts.emplace_back("you are still getting to know them");
    else parts.emplace_back("they are nearly a stranger to you");

    if (affinity >= 0.4F) parts.emplace_back("you like them");
    else if (affinity <= -0.4F) parts.emplace_back("you do not like them");
    else if (affinity <= -0.15F) parts.emplace_back("you are lukewarm about them");

    if (trust >= 0.6F) parts.emplace_back("you trust them");
    else if (trust <= 0.15F) parts.emplace_back("you do not especially trust them");

    if (respect >= 0.6F) parts.emplace_back("you respect their judgement");
    if (admiration >= 0.6F) parts.emplace_back("you look up to them");

    // Last and separately, so a current annoyance never reads as a verdict on the whole
    // relationship.
    if (irritation >= 0.45F) parts.emplace_back("you are annoyed with them right now");
    if (resentment >= 0.4F) parts.emplace_back("something between you is still unresolved");

    std::ostringstream description;
    for (std::size_t index = 0; index < parts.size(); ++index)
    {
        if (index > 0) description << (index + 1 == parts.size() ? ", and " : ", ");
        description << parts[index];
    }
    description << ".";
    return description.str();
}

std::string RelationshipState::Describe() const
{
    std::vector<std::string> parts;
    if (familiarity >= 0.65F) parts.emplace_back("someone she knows well");
    else if (familiarity >= 0.2F) parts.emplace_back("someone she is getting to know");
    else parts.emplace_back("nearly a stranger to her");

    if (affinity >= 0.4F) parts.emplace_back("she likes them");
    else if (affinity <= -0.4F) parts.emplace_back("she does not like them");
    else if (affinity <= -0.15F) parts.emplace_back("she is lukewarm about them");

    if (trust >= 0.6F) parts.emplace_back("she trusts them");
    else if (trust <= 0.15F) parts.emplace_back("she does not especially trust them");

    if (respect >= 0.6F) parts.emplace_back("she respects their judgement");
    if (admiration >= 0.6F) parts.emplace_back("she looks up to them");

    // Stated last and separately, so a current annoyance never reads as a verdict on
    // the whole relationship.
    if (irritation >= 0.45F) parts.emplace_back("she is annoyed with them right now");
    if (resentment >= 0.4F) parts.emplace_back("something between them is still unresolved");

    std::ostringstream description;
    for (std::size_t index = 0; index < parts.size(); ++index)
    {
        if (index > 0) description << (index + 1 == parts.size() ? ", and " : ", ");
        description << parts[index];
    }
    description << ".";
    return description.str();
}

RelationshipState ApplyRelationshipEvent(
    RelationshipState state,
    const RelationshipEvent& event,
    const RelationshipLimits& limits)
{
    // Both scale everything. An unimportant exchange the runtime is unsure about should
    // move a relationship almost not at all.
    const float weight = std::clamp(event.importance, 0.0F, 1.0F) *
        std::clamp(event.confidence, 0.0F, 1.0F);
    if (weight <= 0.0F)
    {
        return state;
    }

    const float positive = std::clamp(event.positiveInteraction, 0.0F, 1.0F);
    const float negative = std::clamp(event.negativeInteraction, 0.0F, 1.0F);
    const float cooperation = std::clamp(event.cooperation, 0.0F, 1.0F);
    const float conflict = std::clamp(event.conflict, 0.0F, 1.0F);

    state.affinity += Bounded(
        (positive - negative) * weight * limits.maximumStep * 2.0F, limits.maximumStep);
    // Trust moves more slowly than liking in both directions, and disrespect costs more
    // than cooperation earns: that asymmetry is what makes trust worth having.
    state.trust += Bounded(
        (event.trustEvidence - event.disrespectEvidence * 1.6F) * weight *
            limits.maximumStep * 2.0F * limits.trustStepScale,
        limits.maximumStep);
    state.respect += Bounded(
        (cooperation - event.disrespectEvidence) * weight * limits.maximumStep,
        limits.maximumStep);
    state.comfort += Bounded(
        (positive - conflict) * weight * limits.maximumStep, limits.maximumStep);
    state.attachment += Bounded(
        positive * weight * limits.maximumStep * 0.5F, limits.maximumStep);
    state.playfulness += Bounded(
        (positive - negative) * weight * limits.maximumStep, limits.maximumStep);
    state.admiration += Bounded(
        event.trustEvidence * weight * limits.maximumStep * 0.5F, limits.maximumStep);

    state.irritation += Bounded(
        (negative + conflict) * weight * limits.maximumStep * 3.0F, limits.maximumStep * 3.0F);
    // Only genuine disrespect accrues as grievance. Ordinary friction does not, or every
    // disagreement would become permanent.
    state.resentment += Bounded(
        event.disrespectEvidence * weight * limits.maximumStep, limits.maximumStep);

    // Contact, not sentiment. Familiarity rises even from an argument, because you do
    // learn about someone by arguing with them.
    state.familiarity = std::clamp(
        state.familiarity + limits.familiarityStep * weight, 0.0F, 1.0F);
    ++state.interactionCount;

    // Warmth cannot outrun acquaintance.
    //
    // Without this the numbers saturate at wildly different rates -- affinity and trust
    // reach 1.0 in about thirty exchanges while familiarity needs hundreds -- and the
    // result is a relationship that describes itself as "nearly a stranger to you, you
    // like them, you trust them". Trust in particular has to be earned over time and
    // not merely earned often.
    //
    // Deliberately one-directional: the ceiling applies to liking, trusting, admiring,
    // and becoming attached, but never to disliking. You can decide you want nothing to
    // do with someone within a minute of meeting them, and pretending otherwise would
    // make hostility from a stranger impossible to register.
    const float acquaintance = 0.25F + 0.75F * std::clamp(state.familiarity, 0.0F, 1.0F);
    state.affinity = std::clamp(state.affinity, -1.0F, acquaintance);
    state.trust = std::clamp(state.trust, 0.0F, acquaintance);
    state.respect = std::clamp(state.respect, 0.0F, 1.0F);
    state.comfort = std::clamp(state.comfort, 0.0F, acquaintance);
    state.attachment = std::clamp(state.attachment, 0.0F, acquaintance);
    state.irritation = std::clamp(state.irritation, 0.0F, 1.0F);
    state.resentment = std::clamp(state.resentment, 0.0F, 1.0F);
    state.admiration = std::clamp(state.admiration, 0.0F, acquaintance);
    state.playfulness = std::clamp(state.playfulness, 0.0F, 1.0F);
    return state;
}

RelationshipState SettleRelationship(
    RelationshipState state,
    const RelationshipLimits& limits)
{
    state.irritation = std::max(0.0F, state.irritation - limits.irritationDecay);
    state.resentment = std::max(0.0F, state.resentment - limits.resentmentDecay);
    // Familiarity, affinity, and trust are untouched. Not seeing someone for a while is
    // not the same as changing your mind about them.
    return state;
}

} // namespace revia::identity
