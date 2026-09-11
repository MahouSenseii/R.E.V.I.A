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

// The boundary an integration lives behind.
//
// Discord, OBS, a game, a browser, a music player -- each is a different world with a
// different protocol, and none of them should be able to change what Revia is allowed to
// do. So a skill is given a deliberately small surface: it can be told what happened, it
// can say what it noticed, and it can *propose* an action. It cannot execute one.
//
// This is the single most important property here, and it is structural rather than
// procedural: nothing in this header hands a skill an executor, a dispatcher, a
// CapabilitySettings it can write to, or an approval registry. A skill that wants
// something done says so and waits, exactly like every other part of Revia that wants
// something done.
//
// There is also only one Revia. A skill has no identity, no mood, no memory and no
// voice of its own -- it contributes events to hers. A "Discord Revia" that behaves like
// a different character is the failure this boundary is shaped to prevent.

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

// An action a skill would like performed.
//
// It is a request in the ordinary sense: it goes through capability policy, desktop
// authorization, rate limiting, approval, and audit exactly as any other action does. A
// skill filling in extra fields cannot change that, because the manager rebuilds the
// parts that decide authority rather than trusting what arrived.
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
