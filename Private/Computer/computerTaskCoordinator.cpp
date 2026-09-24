#include "Computer/computerTaskCoordinator.h"

#include "Computer/learnedPolicy.h"
#include "Computer/routinePolicy.h"
#include "Computer/subgoalValidator.h"
#include "Computer/taskProgression.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace revia::computer
{

ComputerTaskCoordinator::ComputerTaskCoordinator(
    actions::windows::DesktopObserver& observer,
    std::filesystem::path datasetRoot)
    : observations(observer)
    , content(vault)
    , controller(vault)
    , recorder(datasetRoot)
    , defaultDatasetRoot(std::move(datasetRoot))
{
    // Installed here rather than by the session, so that the policy and the vault it
    // borrows are created and destroyed together. A routine policy outliving the vault
    // it holds a pointer to would be a dangling read at the worst possible moment.
    controller.SetRoutinePolicy(std::make_unique<RoutineComputerPolicy>(vault));
    // Installed empty. It reports itself unavailable until an artifact is loaded and
    // accepted, so learned mode is inactive rather than deciding with nothing.
    auto learnedPolicy = std::make_unique<LearnedComputerPolicy>(vault);
    learned = learnedPolicy.get();
    controller.SetLearnedPolicy(std::move(learnedPolicy));
}

ModelCallLedger ComputerTaskCoordinator::Calls() const
{
    const ComputerControllerStats& stats = controller.Stats();
    ModelCallLedger ledger;
    ledger.decisions = stats.decisions;
    ledger.decidedWithoutModel = stats.routineDecisions + stats.learnedDecisions;
    ledger.decisionCalls = stats.modelCalls;
    ledger.subgoalCalls = subgoalRequests;
    ledger.subgoalRefusals = subgoalRefusals;
    ledger.derivedSubgoals = derivedSubgoals;
    return ledger;
}

const LearnedArtifact& ComputerTaskCoordinator::Artifact() const
{
    static const LearnedArtifact none;
    return learned ? learned->Artifact() : none;
}

void ComputerTaskCoordinator::SetLegacyPolicy(std::unique_ptr<IComputerPolicy> policy)
{
    controller.SetLegacyPolicy(std::move(policy));
}

void ComputerTaskCoordinator::SetSubgoalPlanner(SubgoalPlannerCall planner)
{
    subgoalPlanner = std::move(planner);
}

void ComputerTaskCoordinator::ApplySettings(const computerControlSettings& settings)
{
    configured = settings;
    // The configured directory, resolved against the root the coordinator was built
    // with so a relative setting stays inside the runtime data tree. Refused silently
    // while capturing, which is the right outcome: the session in progress keeps its
    // destination and the new one takes effect the next time a session opens.
    if (!settings.datasetDirectory.empty())
    {
        const std::filesystem::path configuredRoot(settings.datasetDirectory);
        static_cast<void>(recorder.SetRoot(configuredRoot.is_absolute()
            ? configuredRoot : defaultDatasetRoot.parent_path() / configuredRoot));
    }
    // The configured artifact, loaded at a task boundary and never mid-run. A refusal
    // leaves the policy unloaded with the reason recorded: learned mode then reports
    // itself inactive and the routine policy keeps deciding, which is the behaviour a
    // missing or unqualified artifact must produce.
    if (learned != nullptr)
    {
        if (settings.learnedArtifactPath.empty())
        {
            learned->Unload();
            artifactRefusal.clear();
        }
        else
        {
            const ArtifactLoad load =
                learned->Load(std::filesystem::path(settings.learnedArtifactPath));
            artifactRefusal = load.accepted ? std::string() : load.detail;
        }
    }
    controller.SetMode(ComputerProviderModeFromString(settings.providerMode));
    controller.SetEscalationBudget(settings.escalationBudget > 0
        ? static_cast<std::uint32_t>(settings.escalationBudget) : 0u);
}

PayloadReference ComputerTaskCoordinator::HoldPayload(std::string value, std::string kind)
{
    activePayload = vault.Store(std::move(value), std::move(kind));
    return activePayload;
}

void ComputerTaskCoordinator::BeginTask(
    const std::string& goalId, const RequestOrigin origin, TaskContent taskContent)
{
    activeGoalId = goalId;
    activeOrigin = origin;
    // Before anything else, and before any planner is asked anything. Exact content is
    // taken into custody here; from this point there is nothing a model could invent
    // that would reach a field, because the gate downstream of every provider replaces
    // whatever it proposed with what is held.
    content.BeginTask(std::move(taskContent));
    if (content.Holds()) activePayload = content.Held();
    lastSubgoalRefusal.clear();
    lastContentRefusal.clear();
    subgoalRequests = 0;
    subgoalRefusals = 0;
    derivedSubgoals = 0;
    lastPhase = TaskPhase::Undetermined;
    lastPhaseDetail.clear();
    controller.ResetStats();
    controller.ClearSubgoal();
    if (learned != nullptr) learned->ClearSubgoal();
    observations.Reset();
    pendingRecord.reset();
    pendingStepCount = 0;
}

void ComputerTaskCoordinator::CompleteTask(const goals::Goal& finished)
{
    FlushPending(finished);
    EndTask();
}

void ComputerTaskCoordinator::EndTask()
{
    // Anything still held is written as it stands. A row whose outcome never arrived
    // says so; dropping it would quietly lose the decisions that ended a run.
    if (pendingRecord.has_value())
    {
        static_cast<void>(recorder.Record(std::move(*pendingRecord)));
        pendingRecord.reset();
    }
    controller.ClearSubgoal();
    observations.Reset();
    content.EndTask();
    // The payloads go with the task that justified holding them. Whatever the outcome:
    // a cancelled task has no more claim on the user's words than a finished one.
    vault.Clear();
    activePayload = PayloadReference{};
    activeGoalId.clear();
    activeOrigin = RequestOrigin::Unknown;
}

bool ComputerTaskCoordinator::InstallSubgoal(
    const ComputerSubgoal& proposed,
    const goals::Goal& goal,
    const ComputerTaskContext& context)
{
    // One validation path, for a model's proposal and the runtime's alike.
    //
    // The runtime does not get a private route to authority. A subgoal it derived is
    // checked against scope, budget, intent requirements and the send-before-placement
    // ordering exactly as one Main produced would be -- which means a derivation with a
    // bug in it is refused rather than trusted for being local.
    SubgoalContext subgoalContext;
    subgoalContext.goalId = goal.id;
    subgoalContext.origin = activeOrigin;
    subgoalContext.scope = goal.scope;
    subgoalContext.actionsLeft = context.actionsLeft;
    subgoalContext.retriesLeft = context.retriesLeft;
    subgoalContext.contentPending = content.Content().Any() && !content.Placed();

    const SubgoalValidation validation =
        ValidateSubgoal(proposed, subgoalContext, vault);
    if (!validation.accepted)
    {
        lastSubgoalRefusal = validation.detail;
        return false;
    }
    if (learned != nullptr) learned->SetSubgoal(validation.subgoal);
    if (!controller.SetSubgoal(validation.subgoal))
    {
        lastSubgoalRefusal = "The validated subgoal was not accepted by the controller.";
        return false;
    }
    lastSubgoalRefusal.clear();
    return true;
}

TaskProgress ComputerTaskCoordinator::Progress(
    const ComputerTaskContext& context, const goals::Goal& goal) const
{
    TaskProgressInputs inputs;
    inputs.content = &content.Content();
    inputs.contentPlaced = content.Placed();
    inputs.submissionReady = content.SubmissionReady();
    inputs.context = &context;
    inputs.payload = content.Held();
    inputs.goalId = goal.id;

    // ContentGate identifies only runtime-guarded commits. A navigation button pressed
    // after typing must not be mistaken for a completed submission.
    inputs.submissionDone = content.SubmissionExecuted();
    // From the goal's approved scope, not from whatever is in front. A window that
    // appears mid-run is not a target, and deriving an operation against it would be
    // treating the desktop as a source of authority.
    if (!goal.scope.approvedApplications.empty())
    {
        inputs.application = goal.scope.approvedApplications.front();
    }
    return DeriveTaskProgress(inputs);
}

void ComputerTaskCoordinator::EnsureSubgoal(
    const ComputerTaskContext& context,
    const goals::Goal& goal,
    std::stop_token stopToken)
{
    if (controller.HasSubgoal()) return;
    if (stopToken.stop_requested()) return;

    // The runtime's own progression, before anything is asked of anyone.
    //
    // This is the change TASK-REVIA-0065 is about. Where task state determines the next
    // operation -- the window is not in front; the content is held and the field is
    // identified; the content has landed and the request asked for it to be sent -- the
    // runtime derives the subgoal and installs it. No call is made, and no model gets
    // the opportunity to rediscover a fact that was already written down and get it
    // wrong, which is what it did: `resolve_target` twice and "Send" once on a task
    // whose content was sitting in the vault.
    const TaskProgress progress = Progress(context, goal);
    lastPhase = progress.phase;
    lastPhaseDetail = progress.detail;
    if (progress.derivable && InstallSubgoal(progress.proposed, goal, context))
    {
        ++derivedSubgoals;
        return;
    }

    if (!subgoalPlanner) return;

    // Everything else. A destination that matches nothing, or matches two things, is a
    // question about meaning and it is the question a model is actually good at -- so it
    // is the one that is worth paying for.
    ++subgoalRequests;
    // The redacted description, for the same reason the decision context uses one: the
    // goal's title is the user's own sentence, and a payload held in a vault is not
    // held at all if the task description still spells it out.
    const std::string& task = content.Content().redacted.empty()
        ? goal.title : content.Content().redacted;
    const SubgoalRequest request = FormatSubgoalRequest(task, context, activePayload);
    const responseOutput answer = subgoalPlanner(
        request.instruction, request.situation, request.schema, stopToken);
    if (!answer.bSuccess)
    {
        ++subgoalRefusals;
        lastSubgoalRefusal = answer.reason.empty()
            ? "The subgoal could not be planned." : answer.reason;
        return;
    }

    const ParsedSubgoal parsed = ParseSubgoal(answer.response);
    if (!parsed.succeeded)
    {
        ++subgoalRefusals;
        lastSubgoalRefusal = parsed.error;
        return;
    }

    // Validated through the same path the derived one takes. Origin, scope and budget
    // are read from the run and never from the proposal, which is the line that stops
    // idle work presenting itself as something the user asked for.
    if (!InstallSubgoal(parsed.proposed, goal, context))
    {
        ++subgoalRefusals;
    }
}

void ComputerTaskCoordinator::HoldDecision(const ComputerTaskContext& context)
{
    if (!recorder.Capturing()) return;

    const ComputerDecisionRecord& decision = controller.LastDecision();
    ExperienceRecord record = BuildExperienceRecord(
        context,
        controller.ActiveSubgoal(),
        decision,
        configured.captureDepth == "control_values"
            ? CaptureDepth::ControlValues : CaptureDepth::Structure);
    // The goal and origin come from the task, not from the subgoal, so a decision taken
    // with no subgoal installed is still attributable to the run that took it.
    record.goalId = activeGoalId;
    record.origin = activeOrigin;
    record.action = decision.action;
    record.target = decision.targetName;

    // Which candidate the decision chose, so the mask says what was picked as well as
    // what was on offer. A ranker trained on a mask with no chosen row has no label.
    //
    // Matched by id rather than by name. Marking by name worked only while every
    // candidate had one: once the observer began admitting unlabelled controls
    // (ISSUE-REVIA-0070), every decision about one produced a mask with nothing chosen,
    // and validation refused the whole row as having no label. The id never leaves this
    // function -- it is not written to the row and never becomes a feature.
    const std::vector<ObservedCandidate>& offered = context.observation.candidates;
    for (std::size_t index = 0;
         index < record.candidates.size() && index < offered.size(); ++index)
    {
        record.candidates[index].chosen = !decision.targetId.empty() &&
            offered[index].id == decision.targetId;
    }

    pendingRecord = std::move(record);
    pendingStepIndex = pendingStepCount;
}

void ComputerTaskCoordinator::FlushPending(const goals::Goal& goal)
{
    if (!pendingRecord.has_value()) return;

    // Completed from the runner's own record. The decision does not get to say whether
    // it worked: the runner executed it, verified it against a typed postcondition, and
    // wrote down what the check established. That is the only source here.
    if (pendingStepIndex < goal.steps.size())
    {
        const goals::GoalStep& step = goal.steps[pendingStepIndex];
        if (!step.attempts.empty())
        {
            const goals::StepAttempt& attempt = step.attempts.back();
            pendingRecord->executed = attempt.executed;
            pendingRecord->outcome = attempt.outcome;
            pendingRecord->checkedBy = attempt.checkedBy;
            if (!attempt.failure.empty())
            {
                pendingRecord->failureCategory =
                    attempt.outcome == goals::VerificationOutcome::Failed
                        ? "verification_failed" : "unverified_effect";
            }
        }
    }
    // A decision that proposed no action never became a step, and its row says so: not
    // executed, outcome unknown. Validation refuses it as a label, which is right --
    // there is nothing in it to learn from -- while it stays in the dataset as evidence
    // of how often the policy declined.
    static_cast<void>(recorder.Record(std::move(*pendingRecord)));
    pendingRecord.reset();
}

goals::NextStep ComputerTaskCoordinator::Decide(
    const goals::Goal& goal,
    const std::uint32_t iteration,
    const perceptionSettings& perception,
    std::stop_token stopToken)
{
    // Exactly one look, taken here and shared by everything that reasons about this
    // iteration. A provider observing for itself would bump the process-wide generation
    // and invalidate a target another provider had already chosen correctly.
    ComputerTaskContext context = observations.Build(goal, iteration, perception);

    // What the runtime is holding, described to every provider and revealed to none.
    // A planner that knows a message of this length exists can choose where it goes;
    // one that does not know will write its own, which is the defect.
    context.preparedContent.held = content.Holds();
    context.preparedContent.kind = content.Held().kind;
    context.preparedContent.length = content.Held().length;
    context.preparedContent.destination = content.Content().destination;

    // The last door. Holding the words in a vault achieves nothing while the task
    // description still contains them, and the description is the user's own sentence.
    // Every provider reasons about the redacted one; the goal's title, which is the
    // user's record of what they asked for, is left exactly as they wrote it.
    if (!content.Content().redacted.empty())
    {
        context.subgoal = content.Content().redacted;
    }

    // What the runner has established since the last iteration. Read from the goal's
    // own attempts rather than from anything a provider said about itself, because the
    // question this answers -- has the content actually landed? -- is exactly the one a
    // provider is least able to answer honestly about its own work.
    content.ObserveGoal(goal);

    // Only when a cheaper provider could actually use one. In the default mode this
    // never runs, so installing the feature costs nothing until someone asks for it.
    const ComputerProviderMode mode = controller.Mode();
    if (mode == ComputerProviderMode::Assisted || mode == ComputerProviderMode::Learned ||
        mode == ComputerProviderMode::Shadow)
    {
        EnsureSubgoal(context, goal, stopToken);
    }

    // A content task whose content has landed, with nothing further asked for, is
    // finished -- and the runtime knows it without asking.
    //
    // The evidence is not a provider's opinion: the content requirement came from the
    // person's own request, the placement was established by a typed postcondition
    // against the exact held value, and the request contained no submission verb. Asking
    // a model to confirm that would be paying for a re-derivation of three facts already
    // written down, and offering it the chance to answer wrongly.
    //
    // Deliberately narrow. It fires only for ExactUserContent tasks, only after
    // ContentGate::Placed, and only when no submission was requested. Everything else
    // still ends where it always did, with the next-step path answering the whole-task
    // question.
    if (mode != ComputerProviderMode::Legacy)
    {
        const TaskProgress progress = Progress(context, goal);
        lastPhase = progress.phase;
        lastPhaseDetail = progress.detail;
        if (progress.phase == TaskPhase::Complete)
        {
            goals::NextStep finished;
            finished.hasStep = false;
            finished.finished = true;
            finished.reason = progress.detail;
            FlushPending(goal);
            pendingStepCount = goal.steps.size();
            HoldDecision(context);
            return finished;
        }
    }

    const bool hadSubgoal = controller.HasSubgoal();
    goals::NextStep next = controller.Decide(context, stopToken);

    // A finished subgoal is not a finished goal, and letting one end the run would stop
    // a task the moment its first bounded piece succeeded.
    //
    // So the subgoal is cleared and the question is asked once more, on the *same*
    // snapshot -- nothing executed, so nothing has changed, and re-observing here would
    // invalidate a target for no reason. One retry, never a loop: if the second pass
    // produces no new subgoal, the mode collapses to the existing path and Main answers,
    // including by saying the goal is done. That judgement stays with the reasoning
    // model and the runner's evidence check, where it was.
    if (hadSubgoal && subgoalPlanner &&
        controller.LastDecision().kind == ComputerDecisionKind::ProposeCompletion)
    {
        controller.ClearSubgoal();
        EnsureSubgoal(context, goal, stopToken);
        if (!stopToken.stop_requested())
        {
            next = controller.Decide(context, stopToken);
        }

        // A second completion in a row means the newly planned subgoal was already
        // satisfied too -- most often because Main proposed the same one again, which a
        // small model does when the situation has not visibly changed.
        //
        // Reporting that as the goal being finished is how a run ends after bringing a
        // window to the front. Whether the *task* is done is not a question the subgoal
        // planner is asked; it only ever proposes local work. So the subgoal is dropped
        // and the existing next-step path is asked, which is the one that can answer
        // "finished" about the whole goal -- and can also propose the step that actually
        // makes progress.
        if (!stopToken.stop_requested() &&
            controller.LastDecision().kind == ComputerDecisionKind::ProposeCompletion)
        {
            controller.ClearSubgoal();
            next = controller.Decide(context, stopToken);
        }
    }

    // Everything above chose *what* to do. This decides what may be typed, and it is
    // downstream of all of it on purpose: legacy, routine, learned and the fallback all
    // arrive here, so the guarantee does not depend on each provider remembering it.
    if (next.hasStep)
    {
        const ContentDecision verdict = content.Apply(next.step, context);
        if (!verdict.allowed)
        {
            next.hasStep = false;
            next.finished = false;
            next.needsInput = verdict.needsInput;
            next.reason = verdict.detail;
            if (!verdict.attempted.empty())
            {
                next.reason += " It proposed: \"" + verdict.attempted + "\".";
            }
            lastContentRefusal = next.reason;
        }
        else if (verdict.outcome == ContentOutcome::InventedText ||
            verdict.outcome == ContentOutcome::ModifiedPayload)
        {
            // Allowed, because the right value was put in its place -- and recorded,
            // because a silent repair would hide the fact that a planner still tried.
            lastContentRefusal = verdict.detail + " It proposed: \"" +
                verdict.attempted + "\".";
        }
    }
    else if (next.finished && !content.CompletionAllowed())
    {
        // "I typed something and we are done" is the shape of the original defect, and
        // it survives every repair that only looks at what gets typed. The content this
        // task exists to place has not been seen in the field, so the task is not
        // finished -- whatever the provider believes about it.
        content.NoteRefusedCompletion();
        next.finished = false;
        next.reason = "The task was reported complete, but the content it was for has "
                      "not been seen in the field it was meant for.";
        lastContentRefusal = next.reason;
    }

    // The previous decision's row, completed from what the runner has since recorded
    // about it. This is why the goal arrives here with every attempt on it.
    FlushPending(goal);
    pendingStepCount = goal.steps.size();
    HoldDecision(context);
    return next;
}

std::string ComputerTaskCoordinator::StatusReport() const
{
    std::ostringstream stream;
    const ComputerProviderMode selected = controller.Mode();
    const ComputerProviderMode effective = controller.EffectiveMode();

    stream << "\n========== Computer Control ==========\n";
    stream << "Selected mode:   " << ToString(selected) << "\n";
    stream << "Active mode:     " << ToString(effective) << "\n";
    if (selected != effective)
    {
        stream << "Why not:         " << controller.ModeUnavailableReason() << "\n";
    }
    stream << "Deciding now:    "
           << (controller.ActiveProvider().empty()
                ? std::string("nothing installed") : controller.ActiveProvider())
           << "\n";
    stream << "Subgoal:         "
           << (controller.HasSubgoal() ? "bounded and validated" : "none")
           << "\n";
    if (!lastSubgoalRefusal.empty())
    {
        stream << "Last refusal:    " << lastSubgoalRefusal << "\n";
    }

    const ComputerControllerStats& stats = controller.Stats();
    stream << "\nThis task\n";
    stream << "Decisions:       " << stats.decisions << "\n";
    stream << "Reached a model: " << stats.modelCalls << "\n";
    stream << "Routine:         " << stats.routineDecisions << "\n";
    stream << "Learned:         " << stats.learnedDecisions << "\n";
    stream << "Escalations:     " << stats.escalations << "\n";
    stream << "Reported tokens: " << stats.tokens << "\n";

    // Both sides, always together. A count of offloaded step decisions on its own is
    // half a measurement, and it is the flattering half.
    const ModelCallLedger ledger = Calls();
    stream << "\nModel calls this task\n";
    stream << "For decisions:   " << ledger.decisionCalls << "\n";
    stream << "Derived free:    " << ledger.derivedSubgoals
           << "  (operations the runtime settled from task state)\n";
    if (lastPhase != TaskPhase::Undetermined)
    {
        stream << "Task phase:      " << ToString(lastPhase);
        if (!lastPhaseDetail.empty()) stream << "  -- " << lastPhaseDetail;
        stream << "\n";
    }
    stream << "For subgoals:    " << ledger.subgoalCalls;
    if (ledger.subgoalRefusals > 0)
    {
        stream << "  (" << ledger.subgoalRefusals << " paid for and refused)";
    }
    stream << "\n";
    stream << "For verifying:   " << ledger.verificationCalls
           << "  (every check here is a typed read of the machine)\n";
    stream << "Total:           " << ledger.Total() << "\n";
    stream << "Existing path:   " << ledger.Baseline()
           << "  (one decision call per iteration)\n";
    stream << "Net:             " << (ledger.Net() >= 0 ? "+" : "") << ledger.Net()
           << (ledger.Net() > 0 ? "  calls saved\n"
                : ledger.Net() < 0 ? "  calls spent over the existing path\n"
                                   : "  no change\n");
    if (stats.shadowComparisons > 0)
    {
        stream << "Shadow agreed:   " << stats.shadowAgreements << " of "
               << stats.shadowComparisons << "\n";
    }
    if (stats.payloadRefusals > 0)
    {
        stream << "Payload refusals: " << stats.payloadRefusals << "\n";
    }

    // What the task was allowed to type, and where it came from. Printed whenever the
    // runtime is holding content, because "the message went in" and "a message went in"
    // are the two answers this whole arrangement exists to keep apart.
    const ContentGateStats& contentStats = content.Stats();
    if (content.Content().Any() || contentStats.supplied > 0 ||
        contentStats.inventions > 0)
    {
        stream << "\nContent\n";
        stream << "Requirement:     " << ToString(content.Content().requirement) << "\n";
        stream << "Held:            "
               << (content.Holds()
                    ? ToString(content.Held().provenance) + ", " +
                        std::to_string(content.Held().length) + " characters"
                    : std::string("nothing"))
               << "\n";
        if (!content.Content().destination.empty())
        {
            stream << "Destination:     " << content.Content().destination << "\n";
        }
        stream << "Placed:          " << (content.Placed() ? "yes, and verified" : "no")
               << "\n";
        stream << "Entered exactly: " << contentStats.supplied << "\n";
        stream << "Planner wrote:   " << contentStats.inventions << " invented, "
               << contentStats.modifiedPayloads << " altered\n";
        stream << "Refused:         " << contentStats.wrongDestinations
               << " wrong field, " << contentStats.missingPayloads << " no content, "
               << contentStats.prematureCompletions << " early completion\n";
        if (!lastContentRefusal.empty())
        {
            stream << "Last note:       " << lastContentRefusal << "\n";
        }
    }

    const CaptureStatus capture = recorder.Status();
    stream << "\nRecording\n";
    stream << "Capturing:       " << (capture.capturing ? "yes" : "no") << "\n";
    if (capture.capturing)
    {
        stream << "Session:         " << capture.sessionId << "\n";
        stream << "Application:     " << capture.application << "\n";
        stream << "Depth:           " << ToString(capture.depth) << "\n";
        stream << "Evidence of:     " << ToString(capture.provenance) << "\n";
        stream << "Rows written:    " << capture.recorded << "\n";
        stream << "Rows refused:    " << capture.refused;
        if (capture.lastRefusal != CaptureRefusal::None)
        {
            stream << "  (last: " << ToString(capture.lastRefusal) << ")";
        }
        stream << "\n";
    }

    stream << "\nLearned artifact\n";
    stream << "Configured:      "
           << (configured.learnedArtifactPath.empty()
                ? std::string("none") : configured.learnedArtifactPath)
           << "\n";
    const LearnedArtifact& artifact = Artifact();
    if (artifact.Loaded())
    {
        stream << "Status:          loaded and qualified\n";
        stream << "Behaviour hash:  " << artifact.behaviourHash << "\n";
        stream << "Dataset lineage: " << artifact.datasetLineage << "\n";
        stream << "Trained:         " << artifact.trainedAt << "\n";
        stream << "Qualified for:   ";
        for (const std::string& application : artifact.qualifiedApplications)
        {
            stream << application << ' ';
        }
        stream << "/ ";
        for (const std::string& intent : artifact.qualifiedIntents) stream << intent << ' ';
        stream << "\n";
        // What it actually claims, rather than only that it loaded. An artifact whose
        // held-out set was two examples should not read the same as one whose was two
        // thousand.
        stream << "Held-out:        " << artifact.heldOutExamples << " example(s), "
               << "coverage " << artifact.heldOutCoverage
               << ", accuracy " << artifact.heldOutAccuracy
               << ", confident mistakes " << artifact.heldOutWrongAndConfident << "\n";
    }
    else if (!artifactRefusal.empty())
    {
        stream << "Status:          refused -- " << artifactRefusal << "\n";
    }
    else
    {
        stream << "Status:          "
               << "no qualified artifact is loaded; learned decisions are inactive\n";
    }
    stream << "======================================\n";
    return stream.str();
}

} // namespace revia::computer
