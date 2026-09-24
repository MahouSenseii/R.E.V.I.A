#include "reviaSessionTestAccess.h"
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
    Check(Contains(Access::RunningTask(session), "tidy the downloads folder"),
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
    Check(Access::RunningTask(session).empty() &&
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
    Check(asked.sawStop.load() && Contains(Access::FinishedTask(session), "cancelled"),
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

} // namespace

void RunBackgroundTaskTests()
{
    TestSheKeepsTalkingWhileSheWorks();
    TestStopOrAskingEndsTheTask();
    TestThePromptSaysWhatSheIsDoing();
    std::cout << "Tasks run in the background: she keeps talking, and only Stop or a "
        "request ends them.\n";
}
