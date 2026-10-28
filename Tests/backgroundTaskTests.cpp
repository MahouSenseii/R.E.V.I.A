#include "reviaSessionTestAccess.h"
#include "actionRuntimeTestAccess.h"
#include "Identity/reviaStatePacket.h"

#include <atomic>
#include <iostream>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using revia::runtime::ReviaSession;
using revia::runtime::RuntimeState;
using revia::tests::Check;
using Access = revia::runtime::ReviaSessionTestAccess;

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

void TestDirectGuestProcessRetainsDiagnostics()
{
    revia::actions::ActionOutcome outcome;
    outcome.policy.verdict = revia::actions::PolicyVerdict::Allowed;
    outcome.result.attempted = true;
    outcome.result.guest.emplace();
    auto& receipt = *outcome.result.guest;
    receipt.attempted = true;
    receipt.receiptVerified = true;
    receipt.uncertainEffect = true;
    receipt.exitCode = 1;
    receipt.standardOutput = "running compiler";
    receipt.standardError = "main.cpp:3: error: missing semicolon";
    const auto failed = ReviaSession::FormatActionOutcome(outcome);
    Check(Contains(failed, "Action stopped:") && Contains(failed, "exit=1") &&
            Contains(failed, "receipt_verified=true") && Contains(failed, "uncertain_effect=true") &&
            Contains(failed, receipt.standardOutput) && Contains(failed, receipt.standardError),
        "A direct guest process failure hid its exit status or compiler diagnostics.");

    receipt.succeeded = outcome.result.succeeded = true;
    receipt.uncertainEffect = false;
    receipt.exitCode = 0;
    receipt.standardOutput = std::string(5000, 'x') + "unshown-tail";
    receipt.standardError.clear();
    const auto bounded = ReviaSession::FormatActionOutcome(outcome);
    Check(Contains(bounded, "Action succeeded:") && Contains(bounded, "exit=0") &&
            Contains(bounded, "output_truncated=true") && bounded.size() < 5000 && !Contains(bounded, "unshown-tail"),
        "Guest process display output was unbounded or concealed display truncation.");

    receipt.succeeded = outcome.result.succeeded = false;
    receipt.standardOutput = "partial output";
    receipt.outputTruncated = receipt.timedOut = receipt.cancelled = true;
    const auto cancelled = ReviaSession::FormatActionOutcome(outcome);
    Check(Contains(cancelled, "timed_out=true") && Contains(cancelled, "cancelled=true") &&
            Contains(cancelled, "output_truncated=true"),
        "Guest process display lost provider cancellation, timeout or truncation facts.");
}

// A task that works until it is released or stopped, and says which.
struct HeldTask
{
    std::atomic<bool> release = false;
    std::atomic<bool> sawStop = false;

    std::function<revia::goals::Goal(std::stop_token)> Body(const std::string& title)
    {
        return [this, title](const std::stop_token stopToken)
        {
            revia::goals::Goal goal;
            goal.title = title;
            while (!release.load() && !stopToken.stop_requested())
            {
                std::this_thread::sleep_for(2ms);
            }
            sawStop.store(stopToken.stop_requested());
            goal.status = sawStop.load()
                ? revia::goals::GoalStatus::Cancelled
                : revia::goals::GoalStatus::Succeeded;
            return goal;
        };
    }
};

void TestSheKeepsTalkingWhileSheWorks()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    HeldTask task;
    std::string message;
    Check(Access::LaunchTask(session, "tidy the downloads folder",
              task.Body("tidy the downloads folder"), message) &&
            Contains(message, "background"),
        "A task did not start in the background.");
    Check(session.HasRunningTask() &&
            Contains(Access::RunningTask(session), "tidy the downloads folder"),
        "The state packet does not know what she is working on.");

    const auto status = Access::SubmitOperator(session, "/task");
    Check(Contains(status.text, "tidy the downloads folder") && !task.sawStop.load(),
        "Talking to her while she worked cancelled the task or could not see it.");
    Check(session.State() == RuntimeState::Acting,
        "The turn ended as Idle while a task still ran, so Stop would be unavailable.");

    std::string refused;
    Check(!Access::LaunchTask(session, "another", task.Body("another"), refused) &&
            Contains(refused, "still working"),
        "A second task started alongside the first.");
    Check(Contains(Access::SubmitOperator(session, "/controller").text, "still working"),
        "The controller was changed under a running task.");

    task.release.store(true);
    Access::WaitForTask(session);
    Check(!session.HasRunningTask() && Access::RunningTask(session).empty() &&
            Contains(Access::FinishedTask(session), "succeeded"),
        "A finished task was not recorded as finished.");
    Check(session.State() == RuntimeState::Idle, "She stayed busy after the task ended.");
}

void TestStopOrAskingEndsTheTask()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;

    HeldTask asked;
    std::string message;
    Check(Access::LaunchTask(session, "rename the photos", asked.Body("rename the photos"), message),
        "The first task did not start.");
    Check(Access::SubmitOperator(session, "Revia, cancel the task.").text == "Stopping the task.",
        "Asking her in plain words did not cancel the task.");
    Access::WaitForTask(session);
    Check(!session.HasRunningTask() && asked.sawStop.load() &&
            Contains(Access::FinishedTask(session), "cancelled"),
        "The task kept running after she was asked to stop it.");

    HeldTask stopped;
    Check(Access::LaunchTask(session, "sort the inbox", stopped.Body("sort the inbox"), message),
        "A task could not start after the last one was cancelled.");
    session.RequestStop();
    Access::WaitForTask(session);
    Check(stopped.sawStop.load(), "Stop did not end the background task.");

    Check(Contains(Access::SubmitOperator(session, "/task cancel").text, "not working"),
        "Cancelling with no task running claimed to stop something.");
}

void TestThePromptSaysWhatSheIsDoing()
{
    revia::identity::ReviaStatePacket packet;
    packet.backgroundTask = "'tidy the downloads folder', started under a minute ago";
    const std::string working = revia::identity::RenderStatePacket(packet, false);
    Check(Contains(working, "tidy the downloads folder") &&
            Contains(working, "do not claim it is finished"),
        "The prompt does not tell her about the task she is running.");

    packet.backgroundTask.clear();
    packet.finishedTask = "Goal 'tidy' Failed (budget).";
    Check(Contains(revia::identity::RenderStatePacket(packet, false), "record only"),
        "The prompt does not ground her report of a finished task.");
}

void SelectPrivateTaskAudience(ReviaSession& session, const std::string& audience,
    const std::vector<std::string>& recipients = {})
{
    std::string error;
    Check(session.SetAudience({revia::identity::AudienceKind::Private, audience, 0, recipients}, error), error);
    (void)Access::StudioInput(session, revia::agents::InputSource::Typed);
}

void TestGoalContractsDoNotAdoptLaterConversationContext()
{
    for (const bool switchParticipant : {false, true})
    {
        revia::tests::ScopedTestDirectory directory;
        ReviaSession session(revia::runtime::CompanionPaths(directory.root, {"contract-scope", "Contract scope fixture", "assistant", false}));
        Access::MarkStudioStarted(session, true);
        SelectPrivateTaskAudience(session, "original-task-room");
        if (switchParticipant) Check(!Access::IntroduceTaskSpeaker(session, "Alice").empty(), "Missing task participant.");
        Access::WithGoalAdmission(session, [&](const auto& stamp, const bool admitted)
        {
            Check(admitted, "The original goal conversation was not admitted.");
            Check(Access::ForegroundContract(session) != nullptr, "The initial foreground contract was not admitted.");
            revia::actions::ActionRequest request;
            request.id = revia::actions::NewActionId();
            request.type = revia::actions::ActionType::VmReadTextFile;
            const auto original = Access::ActionContract(session, request, stamp);
            Check(original && original->cancellation.origin.taskId == stamp.taskId && original->cancellation.ancestorTaskIds.empty(),
                "A goal action adopted an unrelated foreground task's cancellation ancestry.");
            if (switchParticipant)
                Check(!Access::IntroduceTaskSpeaker(session, "Bob").empty(), "Missing next participant.");
            else
                SelectPrivateTaskAudience(session, "later-conversation-room");
            Check(Access::ForegroundContract(session) != nullptr, "The later foreground contract was not admitted.");
            Check(!Access::ActionContract(session, request, stamp), "A stale goal action was rebound to a newer conversation.");
            Check(!Access::ContractRefusal(session, *original).empty(), "A stale goal contract still admitted effects.");
            auto foreground = session.Stamp();
            foreground.policyVersion = stamp.policyVersion;
            Check(Access::ActionContract(session, request, foreground) != nullptr,
                "Refusing a stale goal also blocked the current foreground action.");
        });
    }
}

void TestDelayedGoalRetainsItsBackgroundAdmission()
{
    for (const bool inheritBackgroundAdmission : {true, false})
    {
        revia::tests::ScopedTestDirectory directory;
        ReviaSession session(revia::runtime::CompanionPaths(directory.root, {"delayed-admission", "Delayed admission fixture", "assistant", false}));
        Access::MarkStudioStarted(session, true);
        SelectPrivateTaskAudience(session, "background-origin");
        (void)session.Submit("/task");
        std::atomic<bool> release = false;
        std::atomic<bool> admitted = true;
        std::atomic<bool> examined = false;
        std::string message;
        Check(Access::LaunchTask(session, "delayed scope fixture", [&](const std::stop_token stop)
        {
            while (!release.load() && !stop.stop_requested()) std::this_thread::sleep_for(2ms);
            // Only the actual launch token makes this goal a child of the captured task.
            Access::WithGoalAdmission(session, [&](const auto&, const bool current)
                { admitted.store(current); examined.store(true); },
                inheritBackgroundAdmission ? stop : std::stop_token{});
            revia::goals::Goal goal;
            goal.status = revia::goals::GoalStatus::Failed;
            return goal;
        }, message), message);
        SelectPrivateTaskAudience(session, "changed-before-goal-start");
        release.store(true);
        Access::WaitForTask(session);
        Check(examined.load(), "The delayed goal fixture did not examine its registered goal admission.");
        if (inheritBackgroundAdmission)
            Check(!admitted.load(), "A delayed goal replaced its background task's captured audience with the current conversation.");
        else
            Check(admitted.load(), "An unrelated empty-token goal inherited the background task's captured audience.");
    }
}

void TestTaskContextStaysWithItsPrivateAudience()
{
    revia::tests::ScopedTestDirectory directory;
    HeldTask task;
    ReviaSession session(revia::runtime::CompanionPaths(directory.root, {"task-context", "Task context fixture", "assistant", false}));
    Access::MarkStudioStarted(session, true);
    SelectPrivateTaskAudience(session, "task-owner-room", {"owner"});
    (void)session.Submit("/task");
    const std::string sentinel = "private-task-audience-sentinel";
    std::string message;
    Check(Access::LaunchTask(session, sentinel, task.Body(sentinel), message), message);
    const bool initiallyVisible = Contains(Access::RunningTask(session), sentinel);

    SelectPrivateTaskAudience(session, "another-private-room", {"owner"});
    const bool otherAudienceHidden = Access::RunningTask(session).empty();
    const auto privateStatus = session.Submit("/task");
    const bool genericStatus = Contains(privateStatus.text, "another conversation") && !Contains(privateStatus.text, sentinel);
    const auto refusedCommand = session.Submit("/controller");
    std::string duplicateMessage;
    const bool duplicateRefused = !Access::LaunchTask(session, "another task", task.Body("another task"), duplicateMessage);
    const bool refusalHidden = !Contains(refusedCommand.text, sentinel) && !Contains(duplicateMessage, sentinel);
    SelectPrivateTaskAudience(session, "task-owner-room", {"another-recipient"});
    const bool otherRecipientHidden = Access::RunningTask(session).empty();
    SelectPrivateTaskAudience(session, "task-owner-room", {"owner"});
    const bool returnedVisible = Contains(Access::RunningTask(session), sentinel);
    const bool taskPreserved = session.HasRunningTask() && !task.sawStop.load();
    task.release.store(true);
    Access::WaitForTask(session);
    const bool completedVisible = Contains(Access::FinishedTask(session), sentinel);
    SelectPrivateTaskAudience(session, "another-private-room", {"owner"});
    const bool completedHidden = Access::FinishedTask(session).empty();
    SelectPrivateTaskAudience(session, "task-owner-room", {"owner"});
    const bool completedOnReturn = Contains(Access::FinishedTask(session), sentinel);

    Check(initiallyVisible && returnedVisible && completedVisible && completedOnReturn,
        "Returning to the authorizing private audience lost its task context.");
    Check(taskPreserved && !task.sawStop.load(), "Changing conversation audience cancelled the working task.");
    Check(otherAudienceHidden && otherRecipientHidden && completedHidden,
        "Private task context crossed an audience or recipient boundary.");
    Check(duplicateRefused && refusalHidden, "A task refusal exposed another conversation's private task title.");
    Check(genericStatus, "Private task status exposed its title or falsely reported that no task was running.");
}

void TestLateTaskCompletionCannotRestoreRevokedContext()
{
    revia::tests::ScopedTestDirectory directory;
    HeldTask task;
    ReviaSession session(revia::runtime::CompanionPaths(directory.root, {"late-context", "Late context fixture", "assistant", false}));
    Access::MarkStudioStarted(session, true);
    SelectPrivateTaskAudience(session, "late-task-owner");
    (void)session.Submit("/task");
    const std::string sentinel = "private-late-task-sentinel";
    std::string message;
    Check(Access::LaunchTask(session, sentinel, task.Body(sentinel), message), message);
    SelectPrivateTaskAudience(session, "late-task-other");
    task.release.store(true);
    Access::WaitForTask(session);
    const bool hiddenAfterCompletion = Access::FinishedTask(session).empty();
    SelectPrivateTaskAudience(session, "late-task-owner");
    const bool originalAudienceRetained = Contains(Access::FinishedTask(session), sentinel);
    Check(hiddenAfterCompletion, "A late completion restored private task context in a different audience.");
    Check(originalAudienceRetained, "Suppressing a late report discarded the originating audience's task result.");
}

void TestTaskContextStaysWithItsParticipant()
{
    revia::tests::ScopedTestDirectory directory;
    HeldTask task;
    ReviaSession session(revia::runtime::CompanionPaths(directory.root, {"task-person", "Task participant fixture", "assistant", false}));
    Access::MarkStudioStarted(session, true);
    SelectPrivateTaskAudience(session, "shared-desktop-private-room");
    Check(!Access::IntroduceTaskSpeaker(session, "Alice").empty(), "The task owner introduction failed.");
    (void)session.Submit("/task");
    const std::string sentinel = "private-task-participant-sentinel";
    std::string message;
    Check(Access::LaunchTask(session, sentinel, task.Body(sentinel), message), message);
    Check(!Access::IntroduceTaskSpeaker(session, "Bob").empty(), "The next participant introduction failed.");
    const bool hiddenWhileRunning = Access::RunningTask(session).empty();
    task.release.store(true);
    Access::WaitForTask(session);
    const bool hiddenAfterCompletion = Access::FinishedTask(session).empty();
    Check(!Access::IntroduceTaskSpeaker(session, "Alice").empty(), "The returning participant introduction failed.");
    const bool ownerRetainsResult = Contains(Access::FinishedTask(session), sentinel);
    Check(hiddenWhileRunning && hiddenAfterCompletion,
        "A different private participant received the task owner's context.");
    Check(ownerRetainsResult, "The returning task owner lost the result of the persistent task.");
}

void TestTaskProgressStaysWithItsConversation()
{
    revia::tests::ScopedTestDirectory directory;
    HeldTask task;
    ReviaSession session(revia::runtime::CompanionPaths(directory.root, {"task-progress", "Task progress fixture", "assistant", false}));
    Access::MarkStudioStarted(session, true);
    SelectPrivateTaskAudience(session, "progress-owner");
    (void)session.Submit("/task");
    std::string message;
    Check(Access::LaunchTask(session, "progress fixture", task.Body("progress fixture"), message), message);
    const std::string sentinel = "private-task-progress-sentinel";
    std::atomic<unsigned> delivered = 0;
    std::atomic<bool> unstamped = false;
    const auto subscription = session.Events().Subscribe([&](const revia::runtime::RuntimeEvent& event)
    {
        if (event.component == "Goal" && Contains(event.message, sentinel))
        {
            delivered.fetch_add(1);
            if (event.audienceRevision != session.Audience().revision || !event.stamp.SameSession(session.Stamp()) ||
                event.stamp.taskId.empty() || event.stamp.attemptId.empty() || event.stamp.policyVersion == 0)
                unstamped.store(true);
        }
    });
    revia::goals::GoalProgress progress;
    progress.message = sentinel;
    Access::TaskProgress(session, progress);
    const auto originalCount = delivered.load();
    SelectPrivateTaskAudience(session, "progress-other");
    Access::TaskProgress(session, progress);
    const auto otherCount = delivered.load();
    SelectPrivateTaskAudience(session, "progress-owner");
    Access::TaskProgress(session, progress);
    const auto returnedCount = delivered.load();
    task.release.store(true);
    Access::WaitForTask(session);
    session.Events().Unsubscribe(subscription);
    Check(originalCount == 1 && returnedCount == 2,
        "Task progress was lost when its original private conversation was current.");
    Check(otherCount == originalCount && !unstamped.load(),
        "Task progress crossed conversations or was published without its current disclosure stamp.");
}

void TestConversationPreservesTheBackgroundBrowserLookup()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    const auto browser = revia::actions::ActionRuntimeTestAccess::BrowserCancellation(
        Access::Actions(session));
    HeldTask task;
    std::string message;
    Check(Access::LaunchTask(session, "research a topic", task.Body("research a topic"), message),
        "The background browser fixture task did not start.");
    // Use the real cancellation bridge, without a server or a live browser. Its zero
    // port marks cancellation locally and cannot send a shutdown request anywhere.
    std::uint64_t request = browser->BeginRequest(0, "test-only", "goal:fixture");
    const auto status = Access::SubmitOperator(session, "/task");
    const bool typedCancelled = browser->IsCancelled(request);
    browser->EndRequest(request);
    request = browser->BeginRequest(0, "test-only", "goal-check:fixture");
    const auto voice = session.OfferInput("Tell me how your day is going", revia::agents::InputSource::Voice);
    const bool voiceCancelled = browser->IsCancelled(request);
    session.RequestStop();
    Access::WaitForTask(session);
    const bool stopped = browser->IsCancelled(request);
    browser->EndRequest(request);

    Check(Contains(status.text, "research a topic") &&
            voice == revia::agents::InputVerdict::Queued,
        "The cancellation regression did not reach both real input paths.");
    Check(!typedCancelled,
        "A typed conversation command cancelled the background task's browser lookup.");
    Check(!voiceCancelled,
        "Offered conversation input cancelled the background task's browser lookup.");
    Check(stopped && task.sawStop.load(),
        "Explicit Stop did not cancel the background browser lookup and its task.");
}

void TestConversationStillCancelsAnOrdinaryBrowserLookup()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    const auto browser = revia::actions::ActionRuntimeTestAccess::BrowserCancellation(
        Access::Actions(session));
    std::uint64_t request = browser->BeginRequest(0, "test-only");
    (void)Access::SubmitOperator(session, "/task");
    const bool typedCancelled = browser->IsCancelled(request);
    browser->EndRequest(request);

    request = browser->BeginRequest(0, "test-only");
    (void)session.OfferInput("Tell me how your day is going", revia::agents::InputSource::Voice);
    const bool voiceCancelled = browser->IsCancelled(request);
    browser->EndRequest(request);
    Check(typedCancelled && voiceCancelled,
        "New conversation input stopped interrupting browser work when no task was running.");
}

void TestCancellingTheTaskInterruptsItsBrowserLookup()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    const auto browser = revia::actions::ActionRuntimeTestAccess::BrowserCancellation(
        Access::Actions(session));
    HeldTask task;
    std::string message;
    Check(Access::LaunchTask(session, "research a topic", task.Body("research a topic"), message),
        "The browser cancellation fixture task did not start.");
    const std::uint64_t request = browser->BeginRequest(0, "test-only", "goal:fixture");
    const auto cancelled = Access::SubmitOperator(session, "/task cancel");
    Access::WaitForTask(session);
    const bool browserCancelled = browser->IsCancelled(request);
    browser->EndRequest(request);
    Check(cancelled.succeeded && task.sawStop.load() && browserCancelled,
        "Cancelling the task did not interrupt its browser lookup and task token.");
}

void TestConversationCancelsItsOwnLookupWhileATaskRuns()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    const auto browser = revia::actions::ActionRuntimeTestAccess::BrowserCancellation(
        Access::Actions(session));
    HeldTask task;
    std::string message;
    Check(Access::LaunchTask(session, "research a topic", task.Body("research a topic"), message),
        "The foreground browser fixture task did not start.");
    // The background task may be planning while a conversation owns the browser.
    std::uint64_t request = browser->BeginRequest(0, "test-only", "conversation_internet");
    (void)Access::SubmitOperator(session, "/task");
    const bool typedCancelled = browser->IsCancelled(request);
    browser->EndRequest(request);
    request = browser->BeginRequest(0, "test-only", "conversation_internet");
    (void)session.OfferInput("Tell me how your day is going", revia::agents::InputSource::Voice);
    const bool voiceCancelled = browser->IsCancelled(request);
    browser->EndRequest(request);
    session.RequestStop();
    Access::WaitForTask(session);
    Check(typedCancelled && voiceCancelled,
        "A background task prevented new input from interrupting the conversation's own lookup.");
}

void TestBrowserOwnershipUsesTheRuntimeGoalLabels()
{
    revia::actions::internet::VisibleBrowserCancellation browser;
    for (const char* origin : {"goal:fixture", "goal-baseline:fixture", "goal-check:fixture"})
    {
        const auto request = browser.BeginRequest(0, "test-only", origin);
        browser.CancelActive(true);
        Check(!browser.IsCancelled(request),
            std::string("Conversation interrupted a runtime-owned goal request: ") + origin);
        browser.CancelActive();
        Check(browser.IsCancelled(request),
            std::string("Explicit cancellation did not stop a runtime-owned goal request: ") + origin);
        browser.EndRequest(request);
    }
    for (const char* origin : {"conversation_internet", "autonomous_curiosity/fixture", "llm", ""})
    {
        const auto request = browser.BeginRequest(0, "test-only", origin);
        browser.CancelActive(true);
        Check(browser.IsCancelled(request),
            std::string("A foreground or autonomous request was protected as goal work: ") + origin);
        browser.EndRequest(request);
    }
}

void TestChangingCuriosityPreservesTheBackgroundBrowserLookup()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    const auto browser = revia::actions::ActionRuntimeTestAccess::BrowserCancellation(
        Access::Actions(session));
    HeldTask task;
    std::string message;
    Check(Access::LaunchTask(session, "research a topic", task.Body("research a topic"), message),
        "The curiosity preference fixture task did not start.");
    // Keep the real curiosity loop from planning while the fixture changes its
    // preference. The normal input owner keeps busy set until that turn finishes.
    Access::MarkBusy(session);
    Access::StartIdleReviewFixture(session);
    const auto request = browser->BeginRequest(0, "test-only", "goal:fixture");
    const auto changed = Access::SubmitOperator(session, "/set initiative.curiosityEnabled false");
    const bool taskStillRunning = session.HasRunningTask();
    const bool browserCancelled = browser->IsCancelled(request);
    Access::StopIdleReviewFixture(session);
    session.RequestStop();
    Access::WaitForTask(session);
    browser->EndRequest(request);
    Check(changed.succeeded && taskStillRunning && !browserCancelled,
        "Changing curiosity preferences interrupted the background task's browser lookup.");
}

} // namespace

void RunBackgroundTaskTests()
{
    TestDirectGuestProcessRetainsDiagnostics();
    TestGoalContractsDoNotAdoptLaterConversationContext();
    TestDelayedGoalRetainsItsBackgroundAdmission();
    TestTaskContextStaysWithItsPrivateAudience();
    TestLateTaskCompletionCannotRestoreRevokedContext();
    TestTaskContextStaysWithItsParticipant();
    TestTaskProgressStaysWithItsConversation();
    TestSheKeepsTalkingWhileSheWorks();
    TestStopOrAskingEndsTheTask();
    TestConversationPreservesTheBackgroundBrowserLookup();
    TestConversationStillCancelsAnOrdinaryBrowserLookup();
    TestCancellingTheTaskInterruptsItsBrowserLookup();
    TestConversationCancelsItsOwnLookupWhileATaskRuns();
    TestBrowserOwnershipUsesTheRuntimeGoalLabels();
    TestChangingCuriosityPreservesTheBackgroundBrowserLookup();
    TestThePromptSaysWhatSheIsDoing();
    std::cout << "Tasks run in the background: she keeps talking, and only Stop or a "
        "request ends them.\n";
}
