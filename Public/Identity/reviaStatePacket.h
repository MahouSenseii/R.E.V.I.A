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

// The single canonical description of Revia, handed to whichever model is answering.
//
// One packet, one Revia. Reflex, Fast, Main, and Expert all receive the same rendering
// of this structure, because a personality that changes with the tier that happened to
// be selected is not a personality -- it is four of them sharing a name.
//
// Assembled by the runtime and read by the prompt builder. Nothing here is inferred by a
// model: if Revia is irritated, the runtime says so; if she has become less impulsive
// over months, the runtime says that too. The model's job is to reason and express, not
// to invent the psychology it is expressing.
struct ReviaStatePacket
{
    CoreIdentity identity;

    DevelopmentState development;
    emotion::EmotionVector emotion;
    emotion::MoodState mood;

    // The person she is talking to, when it is someone she knows.
    RelationshipState relationship;
    bool hasRelationship = false;

    std::vector<RelevantMemoryLine> memories;

    // What she likes and dislikes. Already selected and bounded by the runtime; the
    // renderer states them, it does not choose them.
    std::vector<Preference> preferences;

    std::string currentInterest;
    std::string unresolvedThought;

    RuntimeSelfKnowledge runtime;

    // Phase 6 will add drives and the current activity here. They are deliberately
    // absent rather than stubbed: an empty DriveState rendered into a prompt would
    // assert that she wants nothing, which is a claim rather than a gap.
};

// Renders the packet into the block the prompt builder receives.
//
// Sections appear only when they carry something real. A development section with no
// drift, or a relationship section for a stranger, would be noise that dilutes the parts
// that matter -- and worse, would assert state that does not exist yet.
//
// Deterministic: the same packet always renders identically, which is what guarantees
// two model tiers cannot be handed different descriptions of the same moment.
[[nodiscard]] std::string RenderStatePacket(const ReviaStatePacket& packet);

} // namespace revia::identity
