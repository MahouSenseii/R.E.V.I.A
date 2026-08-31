#pragma once

#include "Emotion/stimulus.h"
#include "Identity/relationshipEvidence.h"

#include <string>

namespace revia::emotion
{

// Constructs stimuli from things the runtime confirmed happened.
//
// One place, so every source of feeling is built the same way and none of them can
// quietly invent a dimension. Everything here reads typed outcomes or the same
// observable signals the relationship system uses -- what moves a feeling and what moves
// a relationship must not disagree about what was said.

// A conversational turn. Reuses the signals already read for relationship evidence
// rather than classifying the text a second time.
[[nodiscard]] Stimulus BuildConversationStimulus(
    const std::string& entityId,
    const identity::ConversationSignals& signals);

// A goal run that finished. status carries the typed outcome; nothing is inferred from
// the summary text.
[[nodiscard]] Stimulus BuildGoalStimulus(
    bool succeeded,
    bool exhausted,
    bool blocked,
    std::size_t actionsSpent,
    std::size_t retriesSpent,
    const std::string& summary);

// Research or perception that turned something up. Novelty is supplied by the caller
// because only it knows whether this was already known.
[[nodiscard]] Stimulus BuildDiscoveryStimulus(
    const std::string& description,
    float novelty,
    float importance);

} // namespace revia::emotion
