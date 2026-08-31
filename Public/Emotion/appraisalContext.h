#pragma once

#include "Emotion/emotionTypes.h"
#include "Emotion/moodState.h"
#include "Emotion/stimulus.h"
#include "Identity/developmentState.h"
#include "Identity/relationshipState.h"

#include <string>
#include <vector>

namespace revia::emotion
{

// A memory that bears on the event being appraised. Bounded and pre-selected: the whole
// database must never reach an appraisal, or every feeling becomes a retrieval problem.
struct RelevantMemory
{
    std::string summary;
    float importance = 0.5F;
    // How closely it matches the current situation, from whatever retrieval supplied it.
    float similarity = 0.0F;
    // Whether the remembered episode went well. This is what lets "the last three times
    // this happened it failed" temper an otherwise confident reaction.
    float pastValence = 0.0F;
};

// Everything appraisal is allowed to consider, gathered before any emotion is computed.
//
// The point of a context is that the same event does not produce the same feeling twice.
// A failure matters more when the goal mattered; a sharp remark lands differently from
// someone she trusts; a surprise is only a surprise relative to what was expected. All
// of that has to be assembled first, because the model that reads it must not be able to
// go looking for more.
struct AppraisalContext
{
    identity::DevelopmentState development;
    MoodState mood;
    EmotionVector currentEmotion;

    // The other party, when there is one. hasRelationship stays false for events with no
    // one else involved, and appraisal must not invent a relationship to fill the gap.
    identity::RelationshipState relationship;
    bool hasRelationship = false;

    // Classic appraisal axes, derived rather than supplied. Each is 0..1.
    //
    // How much this matched what was anticipated. Low expectedness is what turns an
    // outcome into surprise rather than mere confirmation.
    float expectedness = 0.5F;
    // How much this bears on something she was actually trying to do. An irrelevant
    // failure is a shrug; a relevant one is a setback.
    float goalRelevance = 0.5F;
    float novelty = 0.0F;
    // Whether anything could have been done about it. Low controllability turns
    // frustration into resignation.
    float controllability = 0.5F;
    // How much of it was hers. The single largest difference between frustration and
    // concern about the world.
    float selfResponsibility = 0.0F;
    // Whether anyone was watching, which is what makes a failure embarrassing rather
    // than merely annoying.
    float socialImportance = 0.0F;

    std::vector<RelevantMemory> memories;

    // Ceiling on retrieved memories reaching one appraisal.
    static constexpr std::size_t maximumMemories = 6;
};

// Turns a stimulus plus current state into the bounded context above.
//
// Deliberately a free function over explicit inputs rather than a class with references
// to half the runtime: appraisal features must be reproducible from what was recorded,
// or the training data generated from them describes a situation nobody can reconstruct.
[[nodiscard]] AppraisalContext BuildAppraisalContext(
    const Stimulus& stimulus,
    const identity::DevelopmentState& development,
    const MoodState& mood,
    const EmotionVector& currentEmotion,
    const identity::RelationshipState* relationship = nullptr,
    std::vector<RelevantMemory> memories = {});

} // namespace revia::emotion
