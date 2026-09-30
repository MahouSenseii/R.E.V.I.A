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
