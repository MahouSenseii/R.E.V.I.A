#pragma once

#include "Computer/computerSubgoal.h"
#include "Computer/computerTypes.h"

#include <cstddef>
#include <vector>

namespace revia::computer
{

// Resolution needs no mutation affordance; entry needs editable, interaction invokable.
enum class TargetAffordance
{
    Any,
    Editable,
    Invokable
};

// One candidate, or a reason there is not exactly one.
struct TargetMatch
{
    enum class Outcome
    {
        Found,
        // Nothing matched the description. Distinct from ambiguous: it may mean the
        // target is not there, or that the bounded observation did not reach it, and the
        // second is not evidence of the first.
        NotFound,
        // More than one candidate fits. Never resolved by picking the first -- the whole
        // reason a descriptor is not an id is that a description matching two things is
        // a question, not a choice.
        Ambiguous
    };

    Outcome outcome = Outcome::NotFound;
    // Borrowed from the candidate list that was passed in, and valid only as long as it
    // is. Nothing here owns an observation.
    const ObservedCandidate* candidate = nullptr;
    std::size_t matchCount = 0;
    // True when the match came from an inferred label or from container context rather
    // than from a name the application published. Recorded so a decision can say which
    // kind of evidence it acted on.
    bool usedInference = false;
};

// Checks published name, inferred label, and container in separate evidence tiers.
// A lower tier is considered only when the higher tier found nothing.
[[nodiscard]] TargetMatch MatchTarget(const std::vector<ObservedCandidate>& candidates,
    const TargetDescriptor& wanted, TargetAffordance affordance);

} // namespace revia::computer
