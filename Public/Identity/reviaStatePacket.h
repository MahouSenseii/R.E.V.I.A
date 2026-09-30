#pragma once

#include "Emotion/emotionTypes.h"
#include "Emotion/moodState.h"
#include "Identity/developmentState.h"
#include "Identity/preferenceState.h"
#include "Identity/relationshipState.h"

#include <string>
#include <vector>

namespace revia::identity
{

// The stable part: who she is and what she will not do. Comes from the profile and does
// not change between turns.
struct CoreIdentity
{
    std::string profileId;
    std::string displayName;
    std::string systemPrompt;
};

// A memory as it reaches a prompt: already selected, already bounded, and carrying how
// sure she is of it so she can say "I think" rather than asserting everything equally.
struct RelevantMemoryLine
{
    std::string summary;
    float confidence = 1.0F;
};

// Facts about the running system that Revia is allowed to state as ground truth.
//
// Supplied rather than inferred. Without this she guesses at her own configuration,
// and a confident wrong answer about whether she can reach the internet is worse than
// no answer at all.
struct RuntimeSelfKnowledge
{
    bool aiReviewEnabled = true;
    // Rendered verbatim. Built by the runtime from real capability state.
    std::string capabilityDescription;
};

// Runtime-assembled canonical state rendered identically for every model tier.
// Models express the supplied psychology without inventing identity or emotion.
struct ReviaStatePacket
{
    CoreIdentity identity;

    DevelopmentState development;
    emotion::EmotionVector emotion;
    emotion::MoodState mood;
    // Runtime cause of the current feeling; empty when no recent event explains it.
    std::string feelingCause;

    // The person she is talking to, when it is someone she knows.
    RelationshipState relationship;
    bool hasRelationship = false;
    // When they last spoke, already in words -- "yesterday 19:42", "a few minutes
    // ago". Described by the runtime rather than handed over as a timestamp, for the
    // same reason the memory block states its clock: a model asked to work out how
    // long ago something was produces a confident wrong answer.
    std::string lastSpokeAt;

    std::vector<RelevantMemoryLine> memories;

    // What she likes and dislikes. Already selected and bounded by the runtime; the
    // renderer states them, it does not choose them.
    std::vector<Preference> preferences;

    std::string currentInterest;
    std::string unresolvedThought;

    // Runtime descriptions of current drives and activity; empty values omit unsupported claims.
    std::string wanting;
    std::string currentActivity;
    // A task the user gave her that is running in the background, or one she finished
    // in the last few minutes, so "how's it going?" and "did it work?" have answers.
    std::string backgroundTask;
    std::string finishedTask;
    // Reminders she holds for the user, soonest first.
    std::string reminders;

    RuntimeSelfKnowledge runtime;

};

// Deterministically renders only meaningful packet sections, identically across model tiers.
[[nodiscard]] std::string RenderStatePacket(const ReviaStatePacket& packet, bool includeRuntimeDetails = true);

} // namespace revia::identity
