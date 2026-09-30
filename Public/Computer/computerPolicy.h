#pragma once

#include "Computer/computerTypes.h"

#include <stop_token>
#include <string>

namespace revia::computer
{

// Proposes decisions without execution or permission authority. All proposals
// pass the runner, runtime, policy, confirmation, rate limit, and audit checks.
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

    // Use only the supplied observation; another Observe() invalidates shared targets.
    // Pass the operation's stop token to every blocking dependency.
    [[nodiscard]] virtual ComputerDecision Decide(const ComputerTaskContext& context, std::stop_token stopToken) = 0;

protected:
    IComputerPolicy() = default;
};

} // namespace revia::computer
