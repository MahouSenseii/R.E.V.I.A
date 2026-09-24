#pragma once

#include "Computer/computerController.h"
#include "Computer/contentGate.h"
#include "Computer/experienceRecorder.h"
#include "Computer/learnedPolicy.h"
#include "Computer/observationBuilder.h"
#include "Computer/payloadVault.h"
#include "Computer/subgoalPlanner.h"
#include "Computer/taskProgression.h"
#include "Library/structLibrary.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace revia::computer
{

// Every model call one task made, and what the same task would have cost without any
// of this.
//
// The previous delivery reported "3 of 4 step decisions taken without a model" and let
// that stand as the saving. It is not the saving. A subgoal costs a call, a refused
// subgoal costs a call and buys nothing, an escalation costs the call the cheap
// provider was supposed to avoid, and a re-plan costs another. A feature that spends
// two calls to save three has saved one, and a feature that spends four to save three
// has cost one -- and the only way to tell them apart is to count both sides.
//
// `baseline` is what the existing path would have spent on the same run: one decision
// call per iteration, which is exactly what legacy mode does. The difference is signed
// on purpose.
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
    // Subgoals the runtime derived from task state and installed without asking anyone.
    //
    // Counted separately from `decidedWithoutModel`, which is about step decisions. This
    // is about the *operation*: the window is not in front, or the content is held and
    // the field is identified, and what comes next follows from that. Each one is a call
    // that used to be spent asking a model to rediscover it.
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

// One owner for the computer-task question, extracted from the session.
//
// ReviaSession used to hold all of this: the observation preparation, the decision
// provider, the payloads, the subgoal lifecycle and (had it existed there) the
// recorder. None of it is session lifecycle. A session starts and stops workers, owns
// identity and holds the conversation; deciding what to click is a different
// responsibility with different state and a different reason to change.
//
// This is composition, not a service locator and not a second session pointer. Its
// dependencies arrive through the constructor and two setters, it holds no reference
// back to the session, and it starts nothing, cancels nothing and executes nothing. The
// GoalRunner still owns budgets, retries, verification and the store; ActionRuntime is
// still the single execution pipeline; this owns the question of *who decides* and
// *what is kept*, which previously had no owner at all.
//
// What it deliberately does not become is a god controller. It coordinates four objects
// it owns outright and exposes them rather than wrapping every one of their methods --
// a forwarding layer over four members would be the same monolith with more files.
class ComputerTaskCoordinator
{
public:
    // Asked for one subgoal, given the formatted situation and the current operation's
    // token. Supplied rather than constructed so Computer depends on neither Core nor
    // LLM, and so a test can drive the whole path with a fixture answer and no backend.
    // Instruction, situation and schema, because those are the three things a
    // constrained chat call actually needs. A single-string signature is what allowed
    // this to be wired to the next-step planner, where the instruction was ignored.
    using SubgoalPlannerCall = std::function<responseOutput(
        const std::string& instruction,
        const std::string& situation,
        const std::string& schema,
        std::stop_token)>;

    ComputerTaskCoordinator(
        actions::windows::DesktopObserver& observer,
        std::filesystem::path datasetRoot);

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

    // Take custody of a value the task will need, and get back the only thing a model
    // will ever see of it. One payload per task: more than one would mean a step that
    // fills a form, and a step that fills a form is a workflow rather than a bounded
    // move.
    //
    // Kept for the fixture and the benchmark, which build a task without a request to
    // parse. The ordinary path is BeginTask with the content the runtime extracted,
    // because a payload held after planning has begun is a payload some provider has
    // already had the chance to invent around.
    [[nodiscard]] PayloadReference HoldPayload(std::string value, std::string kind);

    // Start and end a task. Ending clears the payloads, forgets the previous screen and
    // drops the subgoal, whatever the outcome -- a payload that outlived its task would
    // be user content kept with nothing left to justify keeping it.
    //
    // `content` is what the runtime read out of the user's own request, before any model
    // was asked anything. That ordering is the guarantee: exact content is in custody
    // before there is anything that could invent a substitute for it.
    void BeginTask(
        const std::string& goalId, RequestOrigin origin, TaskContent content = {});
    // Ends the task and writes the last decision's record, now that the run is over and
    // its outcome is known. Called with the finished goal; `EndTask` is the same thing
    // for a caller that has no goal to hand back.
    void CompleteTask(const goals::Goal& finished);
    void EndTask();

    // One iteration: look once, make sure there is a bounded subgoal if the mode wants
    // one, decide, and record if a capture session is open.
    [[nodiscard]] goals::NextStep Decide(
        const goals::Goal& goal,
        std::uint32_t iteration,
        const perceptionSettings& perception,
        std::stop_token stopToken);

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
    void EnsureSubgoal(
        const ComputerTaskContext& context,
        const goals::Goal& goal,
        std::stop_token stopToken);

    // Validate a proposal and install it. One path, whether the runtime derived it or a
    // model produced it -- the runtime does not get a private route to authority.
    [[nodiscard]] bool InstallSubgoal(
        const ComputerSubgoal& proposed,
        const goals::Goal& goal,
        const ComputerTaskContext& context);

    // Where the task has got to, from evidence the runtime already holds.
    [[nodiscard]] TaskProgress Progress(
        const ComputerTaskContext& context, const goals::Goal& goal) const;

    // A decision is recorded when its outcome is known, not when it is made.
    //
    // At decision time the row is half a row: it says what was on screen and what was
    // chosen, and has nothing to say about whether it worked -- which is the only part
    // that decides whether the row may ever be a training label. So the row waits here
    // until the runner has executed and verified the step, and is completed from the
    // goal's own record rather than from anything the decision claimed about itself.
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
