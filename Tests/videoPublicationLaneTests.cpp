#include "../Private/Runtime/videoPublicationLane.h"

#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <stop_token>

namespace
{
using namespace std::chrono_literals;

void Check(const bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void TestRetirementReturnsWithoutThePublicationLane()
{
    for (const bool cancel : {true, false})
    {
        std::mutex mutex;
        std::unique_lock held(mutex);
        std::atomic<bool> current{true};
        std::stop_source stop;
        std::promise<void> entered;
        auto worker = std::async(std::launch::async,
            [&]
            {
                entered.set_value();
                auto lane = revia::runtime::AcquireVideoPublicationLane(mutex, [&] { return current.load() && !stop.stop_requested(); });
                return lane.owns_lock();
            });
        entered.get_future().wait();
        Check(worker.wait_for(30ms) == std::future_status::timeout, "The worker bypassed a currently held publication lane.");
        const auto started = std::chrono::steady_clock::now();
        if (cancel)
            stop.request_stop();
        else
            current.store(false);
        const bool retiredWhileHeld = worker.wait_for(1000ms) == std::future_status::ready;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        // Release even on RED, so the deliberately old blocking implementation
        // can finish and the regression reports a failure instead of hanging.
        held.unlock();
        const bool owned = worker.get();
        std::cout << (cancel ? "stop" : "scope") << " retired_while_held=" << retiredWhileHeld << " owns_lane=" << owned
                  << " retirement_ms=" << elapsed << '\n';
        Check(retiredWhileHeld && !owned, "A retired video worker still required the shutdown-owned publication lane.");
    }
}

void TestCurrentWorkWaitsAndAcquiresTheReleasedLane()
{
    std::mutex mutex;
    std::unique_lock held(mutex);
    std::promise<void> entered;
    auto worker = std::async(std::launch::async,
        [&]
        {
            entered.set_value();
            auto lane = revia::runtime::AcquireVideoPublicationLane(mutex, [] { return true; });
            return lane.owns_lock();
        });
    entered.get_future().wait();
    Check(worker.wait_for(30ms) == std::future_status::timeout, "Current work bypassed the occupied lane.");
    held.unlock();
    Check(worker.wait_for(1000ms) == std::future_status::ready && worker.get(), "Current work failed to acquire the released lane.");
}
}

int main()
{
    try
    {
        TestRetirementReturnsWithoutThePublicationLane();
        TestCurrentWorkWaitsAndAcquiresTheReleasedLane();
        std::cout << "Video publication wait preserves lane ownership and yields to retirement.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Video publication lane failure: " << error.what() << '\n';
        return 1;
    }
}
