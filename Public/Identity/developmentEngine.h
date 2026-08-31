#pragma once

#include "Identity/developmentState.h"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::identity
{

// One observation that might, eventually, change who Revia is.
//
// Never applied on its own. A single event moving a personality trait is the failure
// this whole subsystem exists to prevent: it would mean one conversation could argue her
// into being someone else, and "you're actually very obedient" would be self-fulfilling.
struct DevelopmentEvidence
{
    Trait trait = Trait::Curiosity;
    // Which way this observation points. Magnitude is intentionally ignored -- what
    // accumulates is the count of consistent observations, not the size of any one.
    bool increases = true;
    // What was observed, in plain language, so the eventual change can be explained.
    std::string reason;
};

// Accumulates evidence and applies bounded changes when enough of it agrees.
//
// The properties that matter, all enforced here rather than by convention:
//
//  - Slow. A change needs several consistent observations before anything moves.
//  - Bounded. Each applied step is tiny and lifetime drift per trait is capped, so she
//    can change substantially and still be recognisably herself.
//  - Evidence-based. Nothing moves without recorded observations behind it.
//  - Explainable. Every applied change carries its reason and how much evidence backed
//    it, because "she got less impulsive" is otherwise indistinguishable from a bug.
//  - Reversible. Contradicting evidence cancels rather than being ignored, so growth is
//    not a ratchet in one direction.
//  - Directionless. Nothing here prefers calmer, kinder, or more mature. Development
//    that could only sand her down into a polite assistant would not be development.
class DevelopmentEngine
{
public:
    explicit DevelopmentEngine(DevelopmentLimits limits = {});

    // Records an observation. Returns a change only when this was the one that tipped
    // the balance, which is rarely.
    std::optional<DevelopmentChange> Observe(const DevelopmentEvidence& evidence);

    // Evidence accumulated so far, per trait. Positive means it points toward more.
    [[nodiscard]] float PendingEvidence(Trait trait) const;

    // Applies a change to a development state, respecting the lifetime drift cap.
    static DevelopmentState Apply(
        DevelopmentState development,
        const DevelopmentChange& change,
        const DevelopmentLimits& limits = {});

    [[nodiscard]] const DevelopmentLimits& Limits() const { return limits; }
    void Reset();

private:
    mutable std::mutex mutex;
    DevelopmentLimits limits;
    // Signed running total per trait. Contradicting observations subtract, so a habit
    // she has grown out of stops pulling.
    std::map<Trait, float> pending;
    std::map<Trait, std::size_t> observations;
    std::map<Trait, std::string> lastReason;
};

// Reads development evidence out of a finished turn and its outcome.
//
// Deliberately narrow. Only a handful of things are observable often enough and clearly
// enough to justify moving a personality, and inventing more would mean inferring
// character from noise.
struct TurnObservation
{
    bool succeeded = true;
    // She chose the approach rather than being told exactly what to do.
    bool actedIndependently = false;
    // The attempt was impulsive: acted before checking.
    bool actedImpulsively = false;
    // The user had to correct her again.
    bool wasCorrected = false;
    // The exchange was warm.
    bool wasSocialAndPositive = false;
    // Something new was pursued out of interest rather than instruction.
    bool followedCuriosity = false;
};

[[nodiscard]] std::vector<DevelopmentEvidence> ReadDevelopmentEvidence(
    const TurnObservation& observation);

} // namespace revia::identity
