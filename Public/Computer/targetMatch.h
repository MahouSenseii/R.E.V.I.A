#pragma once

#include "Computer/computerSubgoal.h"
#include "Computer/computerTypes.h"

#include <cstddef>
#include <vector>

namespace revia::computer
{

// What a target has to be able to do, which is a property of the operation and not of
// the control.
//
// Three values rather than a bool, because there are three operations. Entering content
// needs a field that takes text; pressing something needs a control that can be invoked;
// and *resolving* -- finding a control and reporting it -- needs neither, because it
// touches nothing. A bool collapsed the third into the second and made "find the Compose
// box" unanswerable: an edit field is not invokable, so the search that was supposed to
// read the screen refused to see it.
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

// Match a descriptor against what is on screen, in tiers of evidence.
//
// A published name is what the application asserted about itself. An inferred label is
// what UI Automation reasoned about the layout. A container is where the thing sits.
// These are not interchangeable, and collapsing them into one score would let a guess
// outvote a statement -- so each tier is searched on its own, and a tier is consulted
// only when the one above it found nothing at all.
//
// Free, and public, for one reason: it is the same question the routine policy asks when
// it acts and the runtime asks when it decides whether a model call is needed at all. A
// runtime that resolved destinations with its own copy of this would eventually disagree
// with the policy that executes them, and the disagreement would show up as a decision
// aimed at a control the executor then refused -- or worse, did not.
[[nodiscard]] TargetMatch MatchTarget(
    const std::vector<ObservedCandidate>& candidates,
    const TargetDescriptor& wanted,
    TargetAffordance affordance);

} // namespace revia::computer
