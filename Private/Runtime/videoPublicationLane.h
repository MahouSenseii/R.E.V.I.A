#pragma once

#include <chrono>
#include <mutex>
#include <thread>

namespace revia::runtime
{
template <typename Admission> std::unique_lock<std::mutex> AcquireVideoPublicationLane(std::mutex& mutex, const Admission& admitted)
{
    std::unique_lock lane(mutex, std::defer_lock);
    while (admitted())
    {
        if (lane.try_lock())
            break;
        // Shutdown can own this lane while joining the media task. Retirement
        // must release the waiter without requiring the lane to become free.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return lane;
}
}
