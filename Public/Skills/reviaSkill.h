#pragma once

#include "Actions/actionTypes.h"
#include "Presentation/presentationEvents.h"
#include "Runtime/runtimeEvents.h"
#include "Speech/speechCoordinator.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace revia::skills
{

// Skills observe, report events, and propose actions without execution or approval access.
// They contribute to the shared identity, memory, mood, and voice.

// What a skill says it can do. Advisory, for routing and diagnostics; it grants nothing.
struct SkillCapabilities
{
    // Reads state from somewhere and reports it.
    bool observes = false;
    // Wants to propose actions.
    bool proposesActions = false;
    // Has something worth saying out loud from time to time.
    bool speaks = false;
    // Human-readable, for a settings screen.
    std::string description;
};

// Something a skill noticed. The raw material for autonomy evidence, and never a command.
struct SkillObservation
{
    std::string skillId;
    // A short label, safe to show.
    std::string subject;
    // Something finished that she was waiting on.
    bool waitEnded = false;
    // A repeated failure worth reviewing.
    bool repeatedFailure = false;
    // Worth mentioning to the user, if she decides it is.
    bool worthSaying = false;
    // 0..1, how much this matters. Advisory input to scheduling, not a priority it can set.
    float importance = 0.0F;
};

// The manager rebuilds authority fields; proposals retain normal policy,
// desktop authorization, rate limits, approval, and audit checks.
struct SkillProposal
{
    std::string skillId;
    actions::ActionRequest request;
    // Why, in one line, for the audit record and for the user.
    std::string rationale;
};

// What a skill is told. Read-only: a copy of things that already happened.
struct SkillEvent
{
    enum class Kind
    {
        RuntimeState,
        UserPresent,
        UserAway,
        GoalFinished,
        SpeechFinished,
        External
    };

    Kind kind = Kind::External;
    std::string subject;
    std::string payload;
};

class IReviaSkill
{
public:
    virtual ~IReviaSkill() = default;

    [[nodiscard]] virtual std::string Id() const = 0;
    [[nodiscard]] virtual SkillCapabilities Capabilities() const = 0;

    // Lifecycle. Start returns false when the integration is unavailable, which is an
    // ordinary outcome rather than an error: a Discord skill with no token is simply off.
    virtual bool Start(std::string& outError) = 0;
    virtual void Stop() = 0;

    // Told what happened. Must not block.
    virtual void HandleEvent(const SkillEvent& event) = 0;

    // What it would like done right now, if anything. Called by the manager; the skill
    // does not get to push.
    [[nodiscard]] virtual std::vector<SkillProposal> AvailableActions() = 0;

    // What it noticed. Drained the same way.
    [[nodiscard]] virtual std::vector<SkillObservation> Observations() = 0;
};

using SkillHandle = std::shared_ptr<IReviaSkill>;

} // namespace revia::skills
