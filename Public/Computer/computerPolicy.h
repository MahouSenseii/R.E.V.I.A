#pragma once

#include "Computer/computerTypes.h"

#include <stop_token>
#include <string>

namespace revia::computer
{

// One answer to "what should she do next on the machine?".
//
// A policy proposes. It has no execution authority of any kind and is given none: it
// receives a ComputerTaskContext, which is data, and returns a ComputerDecision, which
// is also data. It cannot reach synthesized input, UI Automation mutation, process
// launch, the filesystem or the network; it cannot grant a permission, mint an
// approval, mark itself authorized or weaken a verification. Everything it proposes
// passes GoalRunner::ValidateStep, ActionRuntime::ExecuteScoped, capability policy,
// confirmation, rate limiting and the audit log exactly as a step proposed any other
// way does.
//
// It is not a second agent and not a second personality. It decides how much
// computation a routine desktop decision is worth. It does not decide who Revia is,
// and it never writes her profile, memory, relationships or affect.
class IComputerPolicy
{
public:
    virtual ~IComputerPolicy() = default;

    IComputerPolicy(const IComputerPolicy&) = delete;
    IComputerPolicy& operator=(const IComputerPolicy&) = delete;

    // A short stable name, for the goal record and the diagnostics panel.
    [[nodiscard]] virtual std::string Name() const = 0;

    // Whether this policy can run at all right now: artifact loaded, runtime present,
    // backend reachable. A policy that cannot run says so rather than answering
    // CannotHandle to everything, because "unavailable" and "out of my depth" want
    // different responses from the coordinator.
    [[nodiscard]] virtual bool IsAvailable() const = 0;

    // Exactly one decision, from exactly the observation it was handed.
    //
    // The context is const and the observation inside it is the only look taken this
    // iteration. An implementation must not observe for itself; see
    // ComputerObservation for why that would break a target another provider chose
    // correctly.
    //
    // The stop token is the current operation's. An implementation that waits on
    // anything -- a model, a lease, a file -- passes it down, because a Stop has to end
    // the wait rather than merely be noticed after it.
    [[nodiscard]] virtual ComputerDecision Decide(
        const ComputerTaskContext& context,
        std::stop_token stopToken) = 0;

protected:
    IComputerPolicy() = default;
};

} // namespace revia::computer
