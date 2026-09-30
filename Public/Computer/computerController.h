#pragma once

#include "Computer/computerPolicy.h"
#include "Computer/computerSubgoal.h"
#include "Computer/payloadVault.h"
#include "Computer/routinePolicy.h"
#include "Goals/goalRunner.h"

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>

namespace revia::computer
{

// Modes select the decision provider; all retain the same validation,
// permission checks, confirmation, and audit path.
enum class ComputerProviderMode
{
    // What the application did before any of this existed. The existing decision path,
    // asked every time, with no other provider consulted and nothing recorded. The
    // default, so that installing this feature changes nothing until someone asks it to.
    Legacy,
    // The existing path still decides and still executes. The others are asked the same
    // question from the same snapshot and their answers are recorded and compared.
    // Nothing they say reaches the machine. This is how a provider earns the right to
    // be trusted before it is trusted.
    Shadow,
    // Routine decisions may execute when the routine policy is confident; everything
    // else falls back to the existing path. This is the mode that actually saves the
    // model call.
    Assisted,
    // As Assisted, with a qualified learned artifact consulted ahead of the routine
    // policy. Unavailable without one: an unqualified artifact leaves the mode
    // inactive rather than quietly deciding.
    Learned
};

[[nodiscard]] std::string ToString(ComputerProviderMode value);
[[nodiscard]] ComputerProviderMode ComputerProviderModeFromString(const std::string& value);

// What happened in one iteration, for the activity feed, the record and the benchmark.
struct ComputerDecisionRecord
{
    std::string provider;
    ComputerDecisionKind kind = ComputerDecisionKind::CannotHandle;
    ComputerReasonCode code = ComputerReasonCode::None;
    std::string detail;
    std::uint32_t tokens = 0;
    bool costReported = false;
    // True when a cheaper provider was asked first, abstained, and the existing path
    // had to answer after all. The cost of an escalation is the thing that decides
    // whether a routine policy is worth having, so it is counted rather than inferred.
    bool escalated = false;
    std::string escalatedFrom;
    // Whether a language model was actually asked. The measurement the feature exists
    // to move.
    bool reachedModel = false;
    // What the decision aimed at, when it proposed an action. Carried so a record can be
    // written without reaching back into a proposal that may not have been authorized.
    actions::ActionType action = actions::ActionType::Unknown;
    // Observed control name; empty for unnamed controls. Automation IDs are
    // used only to join candidates to masks, never as training features.
    std::string targetName;
    // Which candidate this aimed at, for joining the decision to the mask. Recorded in
    // the runtime's memory and deliberately not written into a dataset row.
    std::string targetId;

    // Shadow comparison, populated only in Shadow mode. Recorded after the live
    // decision and incapable of changing it.
    bool shadowEvaluated = false;
    std::string shadowProvider;
    ComputerDecisionKind shadowKind = ComputerDecisionKind::CannotHandle;
    std::string shadowDetail;
    // Whether the shadow would have proposed the same action. Compared on the action
    // and its target, not on the wording.
    bool shadowAgreed = false;
};

// Per-goal totals distinguish actual model calls from routine decisions.
struct ComputerControllerStats
{
    std::uint32_t decisions = 0;
    std::uint32_t routineDecisions = 0;
    std::uint32_t legacyDecisions = 0;
    std::uint32_t learnedDecisions = 0;
    std::uint32_t escalations = 0;
    std::uint32_t modelCalls = 0;
    std::uint32_t tokens = 0;
    std::uint32_t shadowComparisons = 0;
    std::uint32_t shadowAgreements = 0;
    // Decisions refused because a payload reference could not be redeemed. Counted
    // because it is the failure that must never quietly become an empty keystroke.
    std::uint32_t payloadRefusals = 0;

    [[nodiscard]] double ModelCallShare() const
    {
        return decisions == 0 ? 0.0
            : static_cast<double>(modelCalls) / static_cast<double>(decisions);
    }
};

// Owns provider selection, escalation, payload redemption, and NextStep mapping.
// GoalRunner owns execution budgets and verification; ReviaSession owns lifecycle.
class ComputerController
{
public:
    // The vault is borrowed. The controller redeems from it and never writes to it:
    // filling the vault is the session's job, because it is the session that knows
    // which of the user's words are part of the task.
    explicit ComputerController(const PayloadVault& payloadVault);

    ComputerController(const ComputerController&) = delete;
    ComputerController& operator=(const ComputerController&) = delete;

    // The existing decision path. Always installed, because it is the fallback every
    // mode ends at and the baseline every other provider is measured against.
    void SetLegacyPolicy(std::unique_ptr<IComputerPolicy> policy);
    // The deterministic policy. Owned here rather than by the session so that the
    // subgoal it works from and the mode that selects it change together.
    void SetRoutinePolicy(std::unique_ptr<RoutineComputerPolicy> policy);
    // Optional. Absent or unavailable leaves Learned mode inactive rather than
    // silently behaving like Assisted, because "the artifact is not loaded" and "the
    // artifact decided" must not look the same from outside.
    void SetLearnedPolicy(std::unique_ptr<IComputerPolicy> policy);

    // Switched at a task boundary and nowhere else. Mid-run the providers would change
    // under a goal that had already been approved on the strength of how it was going
    // to be decided.
    void SetMode(ComputerProviderMode mode);
    [[nodiscard]] ComputerProviderMode Mode() const { return providerMode; }

    // The validated subgoal the routine policy works from. Refused unless it has been
    // through ValidateSubgoal: a subgoal that has not been validated is a model's
    // opinion, and this is the seam where that distinction is enforced.
    [[nodiscard]] bool SetSubgoal(ComputerSubgoal subgoal);
    void ClearSubgoal();
    [[nodiscard]] bool HasSubgoal() const;
    // The subgoal currently installed, for the record and the activity feed. Default
    // constructed when there is none, which reports itself as unvalidated.
    [[nodiscard]] ComputerSubgoal ActiveSubgoal() const;

    // Whether the mode the settings ask for can actually run right now, and why not.
    // Precise, because "unavailable" covers a missing artifact, a missing runtime, an
    // unqualified artifact and an absent subgoal, and a user told only "unavailable"
    // cannot tell which of those they can fix.
    [[nodiscard]] ComputerProviderMode EffectiveMode() const;
    [[nodiscard]] std::string ModeUnavailableReason() const;

    [[nodiscard]] bool HasPolicy() const { return static_cast<bool>(legacy); }
    [[nodiscard]] std::string ActiveProvider() const;

    // Actions become steps; completion remains subject to runner verification.
    // Other decisions become Undecided. The caller stamps ordinal, origin, and binding.
    [[nodiscard]] goals::NextStep Decide(const ComputerTaskContext& context, std::stop_token stopToken);

    [[nodiscard]] const ComputerDecisionRecord& LastDecision() const { return lastRecord; }
    [[nodiscard]] const ComputerControllerStats& Stats() const { return stats; }
    // Called when a goal ends, whatever its outcome. Totals belong to one run.
    void ResetStats();

    // After this many escalations, stop preferring the cheaper provider for this run.
    void SetEscalationBudget(std::uint32_t budget) { escalationBudget = budget; }

private:
    struct Attempt
    {
        ComputerDecision decision;
        bool usable = false;
    };

    [[nodiscard]] Attempt Ask(IComputerPolicy& policy, const ComputerTaskContext& context, std::stop_token stopToken);

    // Turn a reference into the user's actual words, or refuse the decision.
    [[nodiscard]] bool Redeem(ComputerDecision& decision);

    void RunShadow(const ComputerTaskContext& context, std::stop_token stopToken);

    const PayloadVault* vault = nullptr;
    std::unique_ptr<IComputerPolicy> legacy;
    std::unique_ptr<RoutineComputerPolicy> routine;
    std::unique_ptr<IComputerPolicy> learned;

    ComputerProviderMode providerMode = ComputerProviderMode::Legacy;
    std::uint32_t escalationBudget = 3;
    std::uint32_t escalationsThisRun = 0;

    // What the live decision actually aimed at, kept only long enough for the shadow
    // comparison that follows it. Not the whole decision: a proposal that was never
    // authorized should not be reachable from anywhere afterwards.
    actions::ActionRequest liveAction;

    ComputerDecisionRecord lastRecord;
    ComputerControllerStats stats;
};

} // namespace revia::computer
