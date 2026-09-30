#include "Identity/relationshipState.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace revia::identity
{

namespace
{
    float Bounded(const float value, const float limit)
    {
        return std::clamp(value, -limit, limit);
    }
}

bool RelationshipState::ReadsAsTeasing() const
{
    // Teasing requires familiarity without current friction.
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

    // Keep current annoyance separate from lasting sentiment.
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

    // Keep current annoyance separate from lasting sentiment.
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

RelationshipState ApplyRelationshipEvent(RelationshipState state, const RelationshipEvent& event,
    const RelationshipLimits& limits, const std::int64_t nowEpochSeconds)
{
    // Uncertain or unimportant evidence contributes less.
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
    // Trust grows slowly; disrespect costs more than cooperation earns.
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
    // Ordinary friction does not become a lasting grievance.
    state.resentment += Bounded(
        event.disrespectEvidence * weight * limits.maximumStep, limits.maximumStep);

    // Contact increases familiarity regardless of sentiment.
    state.familiarity = std::clamp(
        state.familiarity + limits.familiarityStep * weight, 0.0F, 1.0F);
    ++state.interactionCount;
    // Use the caller's clock; zero leaves contact timestamps unchanged.
    if (nowEpochSeconds > 0)
    {
        const std::string now = std::to_string(nowEpochSeconds);
        if (state.firstSeenAt.empty())
        {
            state.firstSeenAt = now;
        }
        state.lastSeenAt = now;
    }

    // Positive attachment is capped by acquaintance; dislike is not.
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

RelationshipState SettleRelationship(RelationshipState state, const RelationshipLimits& limits)
{
    state.irritation = std::max(0.0F, state.irritation - limits.irritationDecay);
    state.resentment = std::max(0.0F, state.resentment - limits.resentmentDecay);
    // Settling preserves familiarity and earned sentiment.
    return state;
}

} // namespace revia::identity
