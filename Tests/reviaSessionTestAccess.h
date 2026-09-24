#pragma once

#include "testSupport.h"
#include "Runtime/reviaSession.h"

#include "Computer/legacyLlmPolicy.h"

#include <chrono>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace revia::runtime
{

struct ReviaSessionTestAccess
{
    static speech::SpeechService& Speech(ReviaSession& session) { return session.speechService; }

    static SessionResult GuardTurn(ReviaSession& session, const std::function<SessionResult()>& turn)
    { return session.GuardTurn(turn); }
    static void MarkBusy(ReviaSession& session) { session.busy.store(true); }
    static bool LaunchTask(ReviaSession& session, const std::string& title,
        std::function<goals::Goal(std::stop_token)> execute, std::string& outMessage)
    { return session.LaunchTask(title, std::move(execute), outMessage); }
    static void WaitForTask(ReviaSession& session)
    {
        std::jthread worker;
        {
            std::lock_guard lock(session.taskMutex);
            worker = std::move(session.taskWorker);
        }
        if (worker.joinable()) worker.join();
    }
    static std::string RunningTask(const ReviaSession& session) { return session.DescribeRunningTask(); }
    static std::string FinishedTask(const ReviaSession& session) { return session.DescribeFinishedTask(); }
    static void DeliverReminders(ReviaSession& session, planning::WallClock::time_point now)
    { session.DeliverDueReminders(now); }
    static std::string Reminders(const ReviaSession& session) { return session.DescribeReminders(); }
    static void SetClipboard(ReviaSession& session,
        std::function<std::optional<perception::ClipboardText>()> reader)
    { session.clipboardReader = std::move(reader); }
    static std::string ClipboardReference(ReviaSession& session, const std::string& input)
    { return session.ClipboardReference(input); }
    static void Hear(ReviaSession& session, const speech::RecognitionEvent& event)
    { session.OnRecognitionEvent(event); }
    static std::string TakeOfferedInput(ReviaSession& session) { return session.inputArbiter.Take(); }
    static bool IsBusy(const ReviaSession& session) { return session.busy.load(); }
    static void RunBackgroundLoop(ReviaSession& session, std::stop_token stopToken,
        const std::function<void()>& loop)
    { session.RunBackgroundLoop("Test worker", stopToken, loop); }
    static emotion::EmotionRuntime& Emotions(ReviaSession& session) { return session.emotionRuntime; }
    static AffectController& LegacyAffect(ReviaSession& session) { return session.affectController; }
    static identity::RelationshipRegistry& People(ReviaSession& session) { return session.relationships; }

    static float PendingDevelopment(ReviaSession& session, identity::Trait trait)
    { return session.developmentEngine.PendingEvidence(trait); }

    static void SampleLoad(ReviaSession& session, const resources::UsageSnapshot& usage)
    { session.UpdateResourceLoad(usage); }

    static void StartIdleReviewFixture(ReviaSession& session)
    {
        session.settings.initiative.bEnabled = true;
        session.settings.initiative.bCuriosityEnabled = true;
        session.settings.initiative.bSpontaneousSpeechEnabled = false;
        session.settings.initiative.curiosityCheckSeconds = 1;
        session.settings.initiative.autonomousQuietSeconds = 0;
        session.StartCuriosityLoop();
    }

    static void StopIdleReviewFixture(ReviaSession& session) { session.StopCuriosityLoop(); }
    static void RunIdleActivity(ReviaSession& session, const autonomy::ActivityDecision& decision)
    { session.RunAutonomousActivity(decision, "fixture nomination"); }
    static void AgeIdleBudget(ReviaSession& session)
    {
        std::lock_guard lock(session.autonomyMutex);
        session.lastActivityAt = std::chrono::steady_clock::now() - std::chrono::hours(2);
        for (auto& at : session.recentActivities) at = session.lastActivityAt;
    }

    static void MaintenanceEvery(ReviaSession& session, std::chrono::milliseconds emotionInterval,
        std::chrono::milliseconds relationshipQuiet, std::chrono::milliseconds conversationQuiet)
    {
        session.emotionSettleInterval = emotionInterval;
        session.relationshipQuietInterval = relationshipQuiet;
        session.quietConversationInterval = conversationQuiet;
    }

    static goals::Goal RunGoal(ReviaSession& session, goals::Goal goal, bool cancelBeforeRun = false)
    {
        std::lock_guard lock(session.operationMutex);
        (void)session.BeginOperation();
        if (cancelBeforeRun) session.RequestStop();
        return session.RunGoalUnlocked(std::move(goal));
    }

    static std::filesystem::path ActiveVoiceDirectory(const ReviaSession& session)
    { return session.speechService.ActiveVocalizationDirectory(); }

    static agents::MemoryAgent& Memory(ReviaSession& session)
    { return session.turnCoordinator.Memory(); }

    static void SubmitMemoryEvaluation(ReviaSession& session, std::string input, std::uint64_t turnId)
    { session.turnCoordinator.Memory().Submit(session.router, std::move(input), "",
        agents::ResponseProvenance::NormalGeneration, turnId); }

    static agents::LearnedFindingResult SubmitLearning(ReviaSession& session, memoryDecision decision)
    { return session.turnCoordinator.SubmitLearnedFinding(session.router, std::move(decision)); }

    static agents::LearnedFindingResult SubmitLearning(ReviaSession& session,
        const messageRouter& router, memoryDecision decision)
    {
        return session.turnCoordinator.SubmitLearnedFinding(router, std::move(decision), 47);
    }

    static void IdentitySaveEvery(ReviaSession& session, std::chrono::milliseconds interval)
    {
        session.identitySaveInterval = interval;
    }

    static void RememberIdentity(ReviaSession& session, const std::string& name)
    {
        session.relationships.SetDisplayName(identity::LocalUserEntityId(), name);
        session.relationships.ReinforcePreference("fixture astronomy", true,
            identity::PreferenceSource::Observed);
        auto development = session.relationships.Development();
        development.delta[identity::Trait::Patience] = 0.07F;
        session.relationships.SetDevelopment(development);
        auto mood = session.emotionRuntime.Mood();
        mood.valence = 0.31F;
        session.emotionRuntime.SetMood(mood);
    }

    static std::unique_lock<std::mutex> HoldForeground(ReviaSession& session)
    {
        return std::unique_lock(session.operationMutex);
    }

    static std::stop_token OperationToken(ReviaSession& session)
    {
        return session.CurrentOperationToken();
    }

    static void PrepareActions(ReviaSession& session, const std::filesystem::path& root)
    {
        std::string error;
        const bool initialized = session.actionRuntime.Initialize(
            root / "capabilities.json", root / "session-audit.jsonl", error);
        tests::Check(initialized, error);
        (void)session.BeginOperation();
    }

    static SessionResult Execute(ReviaSession& session, actions::ActionRequest request)
    {
        return session.ExecuteAction(std::move(request));
    }

    static void PrepareOperator(ReviaSession& session, const std::filesystem::path& root,
        goals::GoalRunner::StepProvider provider = {})
    {
        PrepareActions(session, root);
        session.goalStore = goals::GoalStore((root / "goals.db").string());
        if (provider) session.goalRunner.SetStepProvider(std::move(provider));
    }

    static void ConfigureLiveOperatorPlanner(ReviaSession& session, const int port)
    {
        tests::Check(session.config.LoadSettings(session.settings), "Live planner settings did not load.");
        session.settings.llm.port = port;
        session.router.ApplyLLMSettings(session.settings.llm, embeddingSettings{}, aiProfile{});
    }

    // The whole `/operate` path, with the finished goal handed back.
    //
    // Goes through Submit, so the request is parsed, the content is extracted before any
    // model call and the task boundary is opened and closed exactly as it is in use.
    // The goal itself comes back out of the store rather than being threaded through the
    // result, because the store is where the run actually recorded it -- and a harness
    // reading the run's own record is reading the same thing an audit would.
    static goals::Goal OperateRequest(ReviaSession& session, const std::string& request)
    {
        static_cast<void>(SubmitOperator(session, "/operate " + request));
        const std::vector<goals::Goal> recent = session.goalStore.LoadRecent(1);
        return recent.empty() ? goals::Goal{} : recent.front();
    }

    // The same task, stopped before any step runs.
    //
    // Deliberately not `Submit` with a stop requested first: Submit begins a new
    // operation, and beginning one installs a fresh stop source -- so a stop requested
    // beforehand is erased by the very call it was meant to interrupt. The first version
    // of this helper did exactly that and reported a fully executed run as a cancelled
    // one, which the generalization matrix then caught by asserting on the action count.
    // That is the right order for a mistake like this to be found in, and it is worth
    // leaving the reason written down.
    //
    // This goes through the same OperateGoal the other cancellation tests use, which
    // requests the stop *after* the operation exists, and opens the task boundary with
    // the content the request carried so the run is otherwise identical.
    static goals::Goal OperateRequestCancelled(
        ReviaSession& session, const std::string& request)
    {
        goals::Goal goal;
        goal.id = goals::NewGoalId();
        goal.title = request;
        goal.scope = goals::NarrowScopeForGoal(session.Capabilities());
        BeginComputerTask(session, goal.id, computer::ExtractTaskContent(request));
        goals::Goal finished =
            OperateGoal(session, std::move(goal), /*cancelBeforeRun=*/true);
        EndComputerTask(session);
        return finished;
    }

    // Tasks run in the background. This waits for one the input started and reports
    // its outcome, which is what the operator tests assert on.
    static SessionResult SubmitOperator(ReviaSession& session, const std::string& input)
    {
        // Exercise the real input/lock owner without starting model or sensor workers.
        session.started.store(true);
        try
        {
            const std::uint64_t launchedBefore = session.tasksLaunched.load();
            auto result = session.Submit(input);
            if (session.tasksLaunched.load() != launchedBefore)
            {
                WaitForTask(session);
                std::lock_guard lock(session.taskMutex);
                if (session.lastTaskReport)
                {
                    result.succeeded = session.lastTaskReport->status == goals::GoalStatus::Succeeded;
                    result.text = session.lastTaskReport->summary;
                }
            }
            session.started.store(false);
            return result;
        }
        catch (...)
        {
            session.started.store(false);
            throw;
        }
    }

    // The computer-task owner, for the integration tests that drive the real decision
    // path. Reached rather than reconstructed: a test that built its own coordinator
    // would be testing a copy of the wiring instead of the wiring.
    static computer::ComputerTaskCoordinator& ComputerTasks(ReviaSession& session)
    { return session.computerTasks; }

    // Replace the decision providers with scripted ones, so the whole path from a
    // request through a bounded subgoal to an executed step runs with no model and no
    // network.
    // The scripted form takes a one-string subgoal answerer for convenience: a test
    // that supplies the answer has no use for the instruction or the schema, and
    // threading them through every scripted lambda would obscure what each test is
    // actually saying.
    static void ScriptComputerProviders(
        ReviaSession& session,
        std::function<responseOutput(const std::string&, std::stop_token)> subgoalPlanner,
        std::function<responseOutput(const std::string&, std::stop_token)> stepPlanner)
    {
        session.computerTasks.SetSubgoalPlanner(
            [subgoalPlanner = std::move(subgoalPlanner)](
                const std::string&, const std::string& situation, const std::string&,
                std::stop_token stopToken)
            {
                return subgoalPlanner(situation, std::move(stopToken));
            });
        session.computerTasks.SetLegacyPolicy(
            std::make_unique<computer::LegacyLlmComputerPolicy>(std::move(stepPlanner)));
    }

    // What one call to a model cost, recorded as it happens.
    //
    // Deliberately a pass-through and not a script. The prompt that goes out is the one
    // the runtime built and the answer that comes back is the model's -- this only
    // watches. A test that returned canned answers here would be measuring its own
    // fixture, which is exactly the thing a live acceptance run exists to stop doing.
    struct ModelCall
    {
        std::string kind;
        std::size_t promptBytes = 0;
        std::size_t responseBytes = 0;
        std::uint32_t tokens = 0;
        bool tokensReported = false;
        bool succeeded = false;
        long long milliseconds = 0;
        std::string response;
    };

    // Route both decision calls through the real router, recording each one.
    static void InstrumentComputerProviders(
        ReviaSession& session, std::vector<ModelCall>& log)
    {
        const auto record = [&log](
            std::string kind,
            const std::size_t promptBytes,
            const std::chrono::steady_clock::time_point started,
            const responseOutput& answer)
        {
            ModelCall call;
            call.kind = std::move(kind);
            call.promptBytes = promptBytes;
            call.responseBytes = answer.response.size();
            call.tokens = answer.TotalTokens();
            call.tokensReported = answer.bTokensReported;
            call.succeeded = answer.bSuccess;
            call.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            call.response = answer.response;
            log.push_back(std::move(call));
        };

        // Each through its own route, because they are different questions under
        // different grammars. Wrapping both in PlanNextGoalStep is precisely the
        // mistake this instrumentation exists to catch.
        session.computerTasks.SetSubgoalPlanner(
            [&session, record](const std::string& instruction,
                const std::string& situation, const std::string& schema,
                std::stop_token stopToken)
            {
                const auto started = std::chrono::steady_clock::now();
                const responseOutput answer = session.router.PlanComputerSubgoal(
                    instruction, situation, schema, std::move(stopToken));
                record("subgoal", instruction.size() + situation.size(), started, answer);
                return answer;
            });
        session.computerTasks.SetLegacyPolicy(
            std::make_unique<computer::LegacyLlmComputerPolicy>(
                [&session, record](const std::string& prompt, std::stop_token stopToken)
                {
                    const auto started = std::chrono::steady_clock::now();
                    const responseOutput answer =
                        session.router.PlanNextGoalStep(prompt, std::move(stopToken));
                    record("next_step", prompt.size(), started, answer);
                    return answer;
                }));
    }

    static void SetComputerSettings(
        ReviaSession& session, const computerControlSettings& settings)
    {
        session.settings.computerControl = settings;
        session.computerTasks.ApplySettings(settings);
    }

    // The real step provider the session installs, rather than a stand-in. This is what
    // makes an integration test an integration test: the provider under test is the one
    // the constructor wired, complete with the ordinal, requested-by and visual target
    // stamping that happens around it.
    static void UseRealStepProvider(ReviaSession& session)
    {
        session.goalRunner.SetStepProvider(
            [&session](const goals::Goal& goal, const std::uint32_t iteration)
            {
                goals::NextStep next = session.computerTasks.Decide(
                    goal, iteration, session.settings.perception,
                    session.CurrentOperationToken());
                if (!next.hasStep) return next;
                next.step.ordinal = static_cast<std::uint32_t>(goal.steps.size());
                next.step.action.requestedBy = "goal";
                next.step.check.requestedBy = "goal";
                return next;
            });
    }

    // The iterative runner, driven through the session that owns it.
    //
    // Deliberately Operate and not Run: the planned runner reads a fixed vector and
    // never asks a provider anything, so a test that used it would exercise none of the
    // decision path. The approval prompt and the command parsing above it have their own
    // tests; what this reaches is the runner, the provider the constructor installed,
    // and everything the provider calls.
    static goals::Goal OperateGoal(
        ReviaSession& session, goals::Goal goal, const bool cancelBeforeRun = false)
    {
        std::lock_guard lock(session.operationMutex);
        (void)session.BeginOperation();
        if (cancelBeforeRun) session.RequestStop();
        // Exactly what the /operate path does once the person approves the task: a
        // standing yes bounded to this run, and the desktop task approval that raises
        // the consequence ceiling for it. Without both, an ordinary content edit is
        // refused -- correctly -- and the run would be demonstrating the refusal rather
        // than the path.
        session.goalRunner.SeedStandingApproval(actions::RiskLevel::ReversibleWrite, true);
        const auto approval = session.actionRuntime.ApproveDesktopTask(goal.id, false);
        static_cast<void>(approval);
        return session.goalRunner.Operate(
            std::move(goal), session.CurrentOperationToken());
    }

    static void BeginComputerTask(ReviaSession& session, const std::string& goalId,
        computer::TaskContent content = {})
    {
        session.computerTasks.BeginTask(
            goalId, computer::RequestOrigin::UserDirected, std::move(content));
    }

    static void EndComputerTask(ReviaSession& session)
    { session.computerTasks.EndTask(); }

    static void UseDatasetRoot(
        ReviaSession& session, const std::filesystem::path& datasetRoot)
    {
        tests::Check(session.computerTasks.Recorder().SetRoot(datasetRoot),
            "The dataset root could not be set; a capture session is already open.");
    }

    static void CompleteComputerTask(ReviaSession& session, const goals::Goal& finished)
    { session.computerTasks.CompleteTask(finished); }

    static void ObserveActions(ReviaSession& session, actions::ActionRuntime::DispatchObserver observer)
    {
        session.actionRuntime.SetDispatchObserver(std::move(observer));
    }
};

} // namespace revia::runtime
