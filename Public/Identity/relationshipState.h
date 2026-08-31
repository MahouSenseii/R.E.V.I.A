#pragma once

#include <cstdint>
#include <string>

namespace revia::identity
{

// How Revia stands with one specific person.
//
// Per entity, and genuinely independent. Nothing here assumes she likes the primary
// user: affinity starts neutral and is earned or lost like everything else, because a
// companion who is fond of you by construction is not fond of you at all.
//
// These are long-lived. They are not emotions, and the difference is load-bearing: she
// can be furious with someone she trusts completely, and amused by someone she does not
// like. Collapsing the two would make every argument a betrayal.
struct RelationshipState
{
    std::string entityId;
    std::string displayName;

    // How well she knows them. Only ever rises, slowly, with interaction.
    float familiarity = 0.0F;

    // Whether she likes them. -1 dislike .. +1 like, starting genuinely neutral.
    float affinity = 0.0F;
    // Whether their word has held up.
    float trust = 0.25F;
    // Whether she rates their judgement. Separate from liking them.
    float respect = 0.25F;

    // Ease in their company, and how much she seeks it out.
    float comfort = 0.2F;
    float attachment = 0.0F;

    // Current friction, which decays, and accumulated grievance, which does not decay
    // nearly as fast. Keeping them apart is what separates "annoyed right now" from
    // "has not forgiven that yet".
    float irritation = 0.0F;
    float resentment = 0.0F;

    float admiration = 0.0F;
    // How much banter is welcome with this person specifically. Teasing that lands with
    // one person is an insult from another.
    float playfulness = 0.2F;

    std::uint64_t interactionCount = 0;
    std::string firstSeenAt;
    std::string lastSeenAt;

    // A jab from someone she is close to and not currently annoyed with is teasing.
    [[nodiscard]] bool ReadsAsTeasing() const;
    // Short description for the prompt, e.g. "familiar, warm, currently a little
    // annoyed". Bounded and plain: the model receives the state, not the numbers.
    [[nodiscard]] std::string Describe() const;
    // The same summary addressed to Revia herself. The prompt speaks to her in the
    // second person throughout, and a "she" appearing mid-block reads as a description
    // of somebody else.
    [[nodiscard]] std::string DescribeForPrompt() const;
};

// Evidence that something happened between them. Not an assignment.
//
// The model never writes relationship numbers. It cannot: only events reach this, and
// the store converts them into bounded deltas. Otherwise "we're best friends now" in
// the middle of a conversation would be true.
struct RelationshipEvent
{
    std::string entityId;

    float positiveInteraction = 0.0F;
    float negativeInteraction = 0.0F;

    // A kept promise, an accurate warning, an admission of being wrong.
    float trustEvidence = 0.0F;
    float disrespectEvidence = 0.0F;

    float cooperation = 0.0F;
    float conflict = 0.0F;

    // How much this particular exchange counts, and how sure the runtime is it read it
    // correctly. Both scale the resulting delta.
    float importance = 0.5F;
    float confidence = 1.0F;

    std::string description;
};

// Bounds on how fast a relationship may move.
struct RelationshipLimits
{
    // One exchange should barely register. Trust in particular is slow to earn.
    float maximumStep = 0.05F;
    float trustStepScale = 0.6F;
    // Familiarity accrues from contact rather than from sentiment, so it has its own
    // small fixed rate and never falls.
    float familiarityStep = 0.006F;
    // Irritation fades; resentment fades far more slowly, and only decays when nothing
    // is renewing it.
    float irritationDecay = 0.12F;
    float resentmentDecay = 0.01F;
};

// Applies evidence to a relationship. Pure: no clock, no storage, no I/O.
[[nodiscard]] RelationshipState ApplyRelationshipEvent(
    RelationshipState state,
    const RelationshipEvent& event,
    const RelationshipLimits& limits = {});

// Time passing with no contact. Friction cools; grievance mostly does not; familiarity
// is never lost, because forgetting someone you know is a memory problem, not a
// relationship one.
[[nodiscard]] RelationshipState SettleRelationship(
    RelationshipState state,
    const RelationshipLimits& limits = {});

} // namespace revia::identity
