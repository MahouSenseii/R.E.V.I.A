#pragma once

#include "Runtime/affectTypes.h"
#include "Runtime/internalStimulus.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace revia::runtime
{

class AffectController
{
public:
    explicit AffectController(
        std::chrono::milliseconds minimumHold = std::chrono::milliseconds(1500),
        std::chrono::milliseconds decayAfter = std::chrono::seconds(45),
        std::chrono::milliseconds lonelyAfter = std::chrono::minutes(20));

    AffectSnapshot Reset();
    // Input observation happens before generation, so this turn's emotion can actually
    // shape this turn's words. ObserveTurn then incorporates the outcome.
    AffectSnapshot ObserveInput(
        const std::string& userInput,
        const SocialContext& social = {});
    AffectSnapshot ObserveTurn(
        const std::string& userInput,
        const std::string& response,
        bool succeeded,
        const SocialContext& social = {});
    // Something that happened to Revia rather than something said to her, entering the
    // same state machine conversation already uses. Returns no snapshot when the event
    // was not worth feeling, which is the common outcome and not a missed case.
    //
    // Deliberately does not count as an interaction: succeeding at a background task
    // must not make her less lonely, because loneliness is about the user being gone
    // and she cannot keep herself company by finishing a job.
    std::optional<AffectSnapshot> ObserveInternalEvent(const InternalStimulus& stimulus);
    std::optional<AffectSnapshot> Tick();
    AffectSnapshot Current() const;

    // Below this, an event is real but not worth a change of expression.
    static constexpr float internalImportanceFloor = 0.25F;

private:
    struct Candidate
    {
        AffectState state = AffectState::Neutral;
        float intensity = 0.25F;
        std::string reason;
    };

    // What was said. Unchanged and still text-only on purpose: separating the reading of
    // the words from what they mean coming from this person keeps both testable, and
    // keeps one keyword list from having to know about relationships.
    static Candidate Classify(
        const std::string& userInput,
        const std::string& response,
        bool succeeded);
    // What it means, said by this person, today. Pure, and inert for a default context.
    static Candidate ModulateForRelationship(
        Candidate candidate,
        const SocialContext& social);
    // Deterministic and model-free, per the tiering rule: a feeling about an outcome the
    // runtime already knows the shape of does not need a language model to name it.
    static Candidate ClassifyInternal(const InternalStimulus& stimulus);
    AffectSnapshot Apply(
        Candidate candidate,
        std::chrono::steady_clock::time_point now,
        bool countsAsInteraction = true);

    mutable std::mutex mutex;
    AffectSnapshot snapshot;
    std::chrono::steady_clock::time_point lastChanged = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastInteraction = lastChanged;
    std::chrono::milliseconds minimumHold;
    std::chrono::milliseconds decayAfter;
    std::chrono::milliseconds lonelyAfter;
    // Repeated low-valence turns can deepen into melancholy instead of resetting to
    // neutral after each reply. Positive turns reduce the momentum again.
    int negativeMomentum = 0;
};

} // namespace revia::runtime
