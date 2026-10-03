#include "testSupport.h"
#include "Core/questionRelay.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using revia::core::QuestionRelay;
using revia::tests::Check;

// A stand-in for the UI thread's event queue, run only when the test says so.
struct UiQueue
{
    std::mutex mutex;
    std::vector<std::function<void()>> work;
    std::atomic<int> posted = 0;

    QuestionRelay::Post Post()
    {
        return [this](std::function<void()> item)
        {
            std::lock_guard lock(mutex);
            work.push_back(std::move(item));
            ++posted;
        };
    }

    void RunAll()
    {
        std::vector<std::function<void()>> ready;
        {
            std::lock_guard lock(mutex);
            ready.swap(work);
        }
        for (auto& item : ready)
            item();
    }

    bool WaitForPost(const int count)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (posted.load() < count && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
        return posted.load() >= count;
    }
};

void TestAQuestionIsAnsweredOnTheUiThread()
{
    QuestionRelay relay;
    UiQueue ui;
    auto answer = std::async(std::launch::async, [&] { return relay.Ask<int>(ui.Post(), false, [] { return 7; }, -1); });
    Check(ui.WaitForPost(1), "The question never reached the UI thread.");
    ui.RunAll();
    Check(answer.wait_for(5s) == std::future_status::ready && answer.get() == 7, "The UI thread's answer did not reach the worker.");
    Check(relay.Ask<int>(
              ui.Post(), true, [] { return 3; }, -1) == 3 &&
              ui.posted.load() == 1,
        "A question on the UI thread itself was posted to itself instead of asked.");
}

// The hang: the UI thread is joining the worker and will never show its dialog.
void TestShutdownRefusesAQuestionNobodyWillShow()
{
    QuestionRelay relay;
    UiQueue ui;
    std::atomic<bool> asked = false;
    auto answer = std::async(std::launch::async,
        [&]
        {
            return relay.Ask<bool>(
                ui.Post(), false,
                [&]
                {
                    asked = true;
                    return true;
                },
                false);
        });
    Check(ui.WaitForPost(1), "The question never reached the UI thread.");
    relay.Abandon();
    const bool released = answer.wait_for(5s) == std::future_status::ready;
    // Answer it anyway if it is stuck, so a regression fails here instead of hanging.
    if (!released)
        ui.RunAll();
    Check(released, "A worker waiting for approval still waited after shutdown began: this is the hang.");
    Check(!answer.get(), "A question abandoned at shutdown came back approved.");

    // If the UI does get to it later, the question is not shown to nobody.
    ui.RunAll();
    Check(!asked.load(), "A question was shown after its asker had stopped waiting.");
    Check(!relay.Ask<bool>(
              ui.Post(), false, [] { return true; }, false) &&
              ui.posted.load() == 1,
        "A question after shutdown was asked instead of refused at once.");
    Check(!relay.Ask<bool>(ui.Post(), true, [] { return true; }, false), "A question on the UI thread after shutdown was still asked.");
}

void TestANewCompanionCanAskAfterTheOldOneWasAbandoned()
{
    QuestionRelay relay;
    UiQueue ui;
    relay.Abandon();
    relay.Reopen();
    int asked = 0;
    const int answer = relay.Ask<int>(
        ui.Post(), true,
        [&]
        {
            ++asked;
            return 23;
        },
        -1);
    Check(answer == 23 && asked == 1, "A new companion's question remained permanently refused after selection.");
}

void TestOldQueuedQuestionsStayRefusedAfterReopen()
{
    QuestionRelay relay;
    UiQueue ui;
    std::atomic<int> oldAsked = 0;
    auto oldAnswer = std::async(std::launch::async,
        [&]
        {
            return relay.Ask<int>(
                ui.Post(), false,
                [&]
                {
                    ++oldAsked;
                    return 11;
                },
                -1);
        });
    const bool posted = ui.WaitForPost(1);
    relay.Abandon();
    const bool released = oldAnswer.wait_for(5s) == std::future_status::ready;
    if (!released)
        ui.RunAll();
    Check(posted && released, "The outgoing companion's pending question was not released before reopening.");
    const int oldResult = oldAnswer.get();

    relay.Reopen();
    ui.RunAll();
    const bool oldQuestionWasHidden = oldAsked.load() == 0;
    std::atomic<int> newAsked = 0;
    auto newAnswer = std::async(std::launch::async,
        [&]
        {
            return relay.Ask<int>(
                ui.Post(), false,
                [&]
                {
                    ++newAsked;
                    return 23;
                },
                -1);
        });
    const bool newPosted = ui.WaitForPost(2);
    ui.RunAll();
    const bool newReleased = newAnswer.wait_for(5s) == std::future_status::ready;
    if (!newReleased)
        relay.Abandon();
    const int newResult = newAnswer.get();
    relay.Abandon();

    Check(oldResult == -1 && oldQuestionWasHidden,
        "An outgoing companion's queued approval was shown after the new companion reopened questions.");
    Check(newPosted && newReleased && newResult == 23 && newAsked.load() == 1,
        "The new companion's worker question did not receive its fresh UI answer.");
}

void TestAnAnswerFromAnOlderGenerationStaysRefused()
{
    QuestionRelay relay;
    UiQueue ui;
    const bool answer = relay.Ask<bool>(
        ui.Post(), true,
        [&]
        {
            relay.Abandon();
            relay.Reopen();
            return true;
        },
        false);
    Check(!answer, "An answer from an outgoing companion was approved after its dialog reopened the relay.");
}

void TestAQueuedAnswerCannotApproveAfterReopen()
{
    QuestionRelay relay;
    UiQueue ui;
    auto answer = std::async(std::launch::async,
        [&]
        {
            return relay.Ask<bool>(
                ui.Post(), false,
                [&]
                {
                    relay.Abandon();
                    relay.Reopen();
                    return true;
                },
                false);
        });
    const bool posted = ui.WaitForPost(1);
    ui.RunAll();
    const bool released = answer.wait_for(5s) == std::future_status::ready;
    if (!released)
        relay.Abandon();
    const bool result = answer.get();
    relay.Abandon();
    Check(posted && released && !result, "A queued UI answer approved the outgoing companion after its relay generation changed.");
}

} // namespace

void RunQuestionRelayTests()
{
    TestAQuestionIsAnsweredOnTheUiThread();
    TestShutdownRefusesAQuestionNobodyWillShow();
    TestANewCompanionCanAskAfterTheOldOneWasAbandoned();
    TestOldQueuedQuestionsStayRefusedAfterReopen();
    TestAnAnswerFromAnOlderGenerationStaysRefused();
    TestAQueuedAnswerCannotApproveAfterReopen();
    std::cout << "Question relay shutdown, stale-generation refusal, and fresh-companion approvals passed.\n";
}
