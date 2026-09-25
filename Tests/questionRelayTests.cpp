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
            ++posted;
            std::lock_guard lock(mutex);
            work.push_back(std::move(item));
        };
    }

    void RunAll()
    {
        std::vector<std::function<void()>> ready;
        {
            std::lock_guard lock(mutex);
            ready.swap(work);
        }
        for (auto& item : ready) item();
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
    auto answer = std::async(std::launch::async, [&]
    {
        return relay.Ask<int>(ui.Post(), false, [] { return 7; }, -1);
    });
    Check(ui.WaitForPost(1), "The question never reached the UI thread.");
    ui.RunAll();
    Check(answer.wait_for(5s) == std::future_status::ready && answer.get() == 7,
        "The UI thread's answer did not reach the worker.");
    Check(relay.Ask<int>(ui.Post(), true, [] { return 3; }, -1) == 3 && ui.posted.load() == 1,
        "A question on the UI thread itself was posted to itself instead of asked.");
}

// The hang: the UI thread is joining the worker and will never show its dialog.
void TestShutdownRefusesAQuestionNobodyWillShow()
{
    QuestionRelay relay;
    UiQueue ui;
    std::atomic<bool> asked = false;
    auto answer = std::async(std::launch::async, [&]
    {
        return relay.Ask<bool>(ui.Post(), false, [&] { asked = true; return true; }, false);
    });
    Check(ui.WaitForPost(1), "The question never reached the UI thread.");
    relay.Abandon();
    const bool released = answer.wait_for(5s) == std::future_status::ready;
    // Answer it anyway if it is stuck, so a regression fails here instead of hanging.
    if (!released) ui.RunAll();
    Check(released,
        "A worker waiting for approval still waited after shutdown began: this is the hang.");
    Check(!answer.get(), "A question abandoned at shutdown came back approved.");

    // If the UI does get to it later, the question is not shown to nobody.
    ui.RunAll();
    Check(!asked.load(), "A question was shown after its asker had stopped waiting.");
    Check(!relay.Ask<bool>(ui.Post(), false, [] { return true; }, false) &&
            ui.posted.load() == 1,
        "A question after shutdown was asked instead of refused at once.");
    Check(!relay.Ask<bool>(ui.Post(), true, [] { return true; }, false),
        "A question on the UI thread after shutdown was still asked.");
}

} // namespace

void RunQuestionRelayTests()
{
    TestAQuestionIsAnsweredOnTheUiThread();
    TestShutdownRefusesAQuestionNobodyWillShow();
    std::cout << "Approvals asked from a worker are refused at shutdown instead of waiting "
        "on a UI thread that will never show them.\n";
}
