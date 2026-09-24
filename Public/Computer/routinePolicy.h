#pragma once

#include "Computer/computerPolicy.h"
#include "Computer/targetMatch.h"
#include "Computer/computerSubgoal.h"
#include "Computer/payloadVault.h"

#include <cstdint>
#include <string>
#include <vector>

namespace revia::computer

{

// Decisions that do not need a language model, made without one.
//
// The premise of the whole feature in one class. A great many desktop steps are not
// reasoning problems: the subgoal says which application, the observation says whether
// it is in front, and what to do next follows from those two facts and nothing else.
// Sending that to a seven-billion-parameter model is paying reasoning prices for a
// lookup, and the latency shows up as a pause between a person asking for something and
// anything happening.
//
// So this is deliberately not a small model. It is the deterministic floor that a
// learned policy later has to beat: no weights, no inference, no nondeterminism, and an
// answer in microseconds. Section 9's instruction to compare against non-neural
// baselines before adding complexity is easier to honour when the baseline is a real
// component rather than a hypothetical one.
//
// What it will not do is more important than what it will. It abstains -- loudly, by
// name -- on ambiguity, on a target it cannot find, on a stale or withheld observation,
// on anything outside the small set of intents below. Abstention routes the decision
// back to Main, which is the correct outcome and not a failure: a routine policy that
// guesses when it is unsure is a routine policy that has to be switched off.
//
// It has no more authority than any other policy. It proposes a typed step; the step is
// validated, policy-checked, confirmed, rate-limited and audited exactly as one that
// came from a model. The only thing it saves is the call.
class RoutineComputerPolicy final : public IComputerPolicy
{
public:
    // The vault is borrowed, not owned, and is consulted only to confirm that a payload
    // reference is real. This class never sees a payload's value: it proposes a step
    // that *names* the reference, and the runtime substitutes the original after the
    // action has been authorized. A routine policy holding the user's exact words would
    // be a routine policy that could put them somewhere else.
    explicit RoutineComputerPolicy(const PayloadVault& payloadVault);

    [[nodiscard]] std::string Name() const override { return "routine"; }
    // Always. It needs no artifact, no runtime and no backend, which is exactly why it
    // is the fallback that cannot itself fail to load.
    [[nodiscard]] bool IsAvailable() const override { return true; }

    [[nodiscard]] ComputerDecision Decide(
        const ComputerTaskContext& context,
        std::stop_token stopToken) override;

    // The subgoal this policy is working toward.
    //
    // Set by the controller at a task boundary, from a validated subgoal and never from
    // a raw proposal. Without one the policy abstains on everything: a deterministic
    // policy with no typed goal has nothing to be deterministic about, and inferring one
    // from the goal's free-text title is exactly the open-ended reading this design
    // keeps with Main.
    void SetSubgoal(ComputerSubgoal subgoal);
    void ClearSubgoal();
    [[nodiscard]] const ComputerSubgoal& Subgoal() const { return activeSubgoal; }

    // Whether this policy would even try, before it is asked.
    //
    // Used for coverage reporting -- "how many of this run's decisions were routine?" --
    // and by the controller to avoid counting an abstention it could have predicted as
    // an escalation that cost something.
    [[nodiscard]] bool CoversIntent(SubgoalIntent intent) const;

private:
    // The shared matcher, asked with the active subgoal's descriptor.
    //
    // The tiers, the affordance rule and the refusal to pick the first of two live in
    // MatchTarget now. They were private here, and the runtime needs the same answer to
    // decide whether a model call is required at all -- two copies of this would
    // eventually disagree, and the disagreement would surface as a decision aimed at a
    // control the executor then refused.
    [[nodiscard]] TargetMatch FindTarget(
        const ComputerTaskContext& context, TargetAffordance affordance) const;

    const PayloadVault* vault = nullptr;
    ComputerSubgoal activeSubgoal;
};

} // namespace revia::computer
