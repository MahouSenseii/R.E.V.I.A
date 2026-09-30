#pragma once

#include "Computer/computerSettings.h"
#include "LLM/responseTypes.h"
#include "Perception/perceptionSettings.h"
#include "Computer/computerController.h"
#include "Computer/contentGate.h"
#include "Computer/experienceRecorder.h"
#include "Computer/learnedPolicy.h"
#include "Computer/observationBuilder.h"
#include "Computer/payloadVault.h"
#include "Computer/subgoalPlanner.h"
#include "Computer/taskProgression.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace revia::computer
{

// Counts every task model call, including subgoals, refusals, escalations, and replans.
// Baseline is one legacy decision call per iteration; savings may be negative.
struct ModelCallLedger
{
    // Iterations the runner asked for a decision.
    std::uint32_t decisions = 0;
    // Decisions answered without reaching a model.
    std::uint32_t decidedWithoutModel = 0;
    // Model calls spent answering a decision, including every fallback and escalation.
    std::uint32_t decisionCalls = 0;
    // Model calls spent planning subgoals, including the ones that were refused.
    std::uint32_t subgoalCalls = 0;
    // Subgoal proposals that were refused after being paid for.
    std::uint32_t subgoalRefusals = 0;
    // Model calls spent on verification. Zero, and stated rather than omitted: every
    // check in this pipeline is a typed read of the machine, so none of them is a model
    // call. If that ever changes this is where it must be counted.
    std::uint32_t verificationCalls = 0;
    // Runtime-derived subgoals avoid planning calls; counted separately from step decisions.
    std::uint32_t derivedSubgoals = 0;

    [[nodiscard]] std::uint32_t Total() const
    {
        return decisionCalls + subgoalCalls + verificationCalls;
    }
    // What the existing path would have spent: one call per decision.
    [[nodiscard]] std::uint32_t Baseline() const { return decisions; }
    // Positive means fewer calls than the existing path, negative means more.
    [[nodiscard]] int Net() const
    {
        return static_cast<int>(Baseline()) - static_cast<int>(Total());
    }
};

// Owns observations, providers, payloads, subgoals, and recording without a session reference.
// GoalRunner owns budgets and verification; ActionRuntime owns execution.
class ComputerTaskCoordinator
{
public:
    // Supplied planner receives instruction, situation, schema, and the operation's stop token.
    using SubgoalPlannerCall = std::function<responseOutput(
        const std::string& instruction,
        const std::string& situation,
        const std::string& schema,
        std::stop_token)>;

    ComputerTaskCoordinator(actions::windows::DesktopObserver& observer, std::filesystem::path datasetRoot);

    ComputerTaskCoordinator(const ComputerTaskCoordinator&) = delete;
    ComputerTaskCoordinator& operator=(const ComputerTaskCoordinator&) = delete;

    // The existing model-driven decision path. Installed once by the session.
    void SetLegacyPolicy(std::unique_ptr<IComputerPolicy> policy);
    // How a subgoal is asked for. Without one, no subgoal is ever proposed and every
    // mode collapses to the existing path -- which is the correct behaviour and not a
    // failure, because a routine policy with nothing bounded to work toward should not
    // be deciding anything.
    void SetSubgoalPlanner(SubgoalPlannerCall planner);

    // Applied at a task boundary. Mid-run the providers would change under a goal that
    // had already been approved on the strength of how it was going to be decided.
    void ApplySettings(const computerControlSettings& settings);
    [[nodiscard]] const computerControlSettings& Settings() const { return configured; }

    // Holds one payload for fixture or benchmark tasks. Ordinary tasks use BeginTask
    // to take custody of extracted content before any model planning.
    [[nodiscard]] PayloadReference HoldPayload(std::string value, std::string kind);

    // BeginTask takes custody of exact content before planning. EndTask clears
    // payloads, the previous observation, and the subgoal regardless of outcome.
    void BeginTask(const std::string& goalId, RequestOrigin origin, TaskContent content = {});
    // Ends the task and writes the last decision's record, now that the run is over and
    // its outcome is known. Called with the finished goal; `EndTask` is the same thing
    // for a caller that has no goal to hand back.
    void CompleteTask(const goals::Goal& finished);
    void EndTask();

    // One iteration: look once, make sure there is a bounded subgoal if the mode wants
    // one, decide, and record if a capture session is open.
    [[nodiscard]] goals::NextStep Decide(const goals::Goal& goal,
        std::uint32_t iteration, const perceptionSettings& perception, std::stop_token stopToken);

    [[nodiscard]] ComputerObservationBuilder& Observations() { return observations; }
    [[nodiscard]] ComputerController& Controller() { return controller; }
    [[nodiscard]] ComputerExperienceRecorder& Recorder() { return recorder; }
    // Why the configured artifact is not loaded, when it is not. Empty when one is.
    [[nodiscard]] const std::string& ArtifactRefusal() const { return artifactRefusal; }
    [[nodiscard]] const LearnedArtifact& Artifact() const;
    [[nodiscard]] const ComputerControllerStats& Stats() const { return controller.Stats(); }
    // What the content gate did this task. Read by the diagnostics and by the tests
    // that check a planner's invented text never reached a field.
    [[nodiscard]] const ContentGateStats& ContentStats() const { return content.Stats(); }
    [[nodiscard]] const ContentGate& Content() const { return content; }
    // Both sides of the cost, for this task. The one number that can honestly answer
    // "did this save anything".
    [[nodiscard]] ModelCallLedger Calls() const;
    // Where the runtime believes this task has got to, and why. For the activity feed,
    // the diagnostics and the generalization matrix.
    [[nodiscard]] TaskPhase Phase() const { return lastPhase; }
    [[nodiscard]] const std::string& PhaseDetail() const { return lastPhaseDetail; }

    // What the user is shown when they ask. Precise about which of the several possible
    // reasons a mode is inactive actually applies, because a person told only
    // "unavailable" cannot tell which one they can do something about.
    [[nodiscard]] std::string StatusReport() const;

private:
    // Ask Main for one bounded subgoal, validate it, and install it if it holds up.
    void EnsureSubgoal(const ComputerTaskContext& context, const goals::Goal& goal, std::stop_token stopToken);

    // Validate a proposal and install it. One path, whether the runtime derived it or a
    // model produced it -- the runtime does not get a private route to authority.
    [[nodiscard]] bool InstallSubgoal(const ComputerSubgoal& proposed, const goals::Goal& goal, const ComputerTaskContext& context);

    // Where the task has got to, from evidence the runtime already holds.
    [[nodiscard]] TaskProgress Progress(const ComputerTaskContext& context, const goals::Goal& goal) const;

    // Keep the pending decision until the runner reports execution and verification;
    // only that evidence determines its training label.
    void HoldDecision(const ComputerTaskContext& context);
    void FlushPending(const goals::Goal& goal);

    std::optional<ExperienceRecord> pendingRecord;
    // Which step the pending row is about, so its outcome is read from the right one.
    std::size_t pendingStepIndex = 0;
    // How many steps the goal had when the pending decision was taken, which is
    // the index the step it produced will occupy.
    std::size_t pendingStepCount = 0;

    ComputerObservationBuilder observations;
    PayloadVault vault;
    // Downstream of every provider, because the defect it closes was never specific to
    // one of them. Declared after the vault it borrows and before the controller, so
    // the three are destroyed in an order where nothing outlives what it points at.
    ContentGate content;
    ComputerController controller;
    ComputerExperienceRecorder recorder;

    // Where the recorder writes when nothing else is configured. Kept so a relative
    // datasetDirectory can be resolved against the runtime data tree rather than
    // against whatever the process working directory happens to be.
    std::filesystem::path defaultDatasetRoot;

    SubgoalPlannerCall subgoalPlanner;
    computerControlSettings configured;

    std::string activeGoalId;
    RequestOrigin activeOrigin = RequestOrigin::Unknown;
    PayloadReference activePayload;
    // What the content gate last had to say, for the activity feed. A substitution that
    // left no trace would hide the fact that a planner tried to write its own words.
    std::string lastContentRefusal;
    // Why the last subgoal proposal was refused, for the activity feed. A run that
    // silently stops using the routine policy is a run nobody can debug.
    std::string lastSubgoalRefusal;
    // Why the configured learned artifact was refused, kept so the diagnostics can say
    // which of several possible reasons applies rather than only "unavailable".
    std::string artifactRefusal;
    // Borrowed, not owned: the controller owns the policy. Held so the subgoal can be
    // handed to it alongside the routine one.
    LearnedComputerPolicy* learned = nullptr;
    // How many subgoals this task has asked Main for. Counted because a subgoal costs a
    // model call, and a feature that saves calls has to account for the ones it spends.
    std::uint32_t subgoalRequests = 0;
    // How many of those were paid for and then refused. A refused subgoal is the most
    // expensive kind: it costs a call and buys nothing, and the run falls back to the
    // path it was trying to avoid.
    std::uint32_t subgoalRefusals = 0;
    // Subgoals derived from task state rather than asked for. The measurement that says
    // whether moving progression into the runtime actually removed calls.
    std::uint32_t derivedSubgoals = 0;
    // Where the runtime believes the task has got to, and why. Diagnostics only: the
    // phase is recomputed from the observation every iteration and never cached as
    // authority, because a stale phase would be a decision made about an old screen.
    TaskPhase lastPhase = TaskPhase::Undetermined;
    std::string lastPhaseDetail;
};

} // namespace revia::computer
