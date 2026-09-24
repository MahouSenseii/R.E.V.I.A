#include "LLM/inferenceScheduler.h"

#include <algorithm>
#include <utility>

namespace revia::llm
{

InferenceScheduler::Lease::Lease(
    InferenceScheduler* inputOwner,
    const InferencePriority inputPriority,
    std::shared_ptr<std::stop_source> inputPreemptionSource)
    : owner(inputOwner),
      priority(inputPriority),
      preemptionSource(std::move(inputPreemptionSource))
{
}

InferenceScheduler::Lease::~Lease()
{
    Reset();
}

InferenceScheduler::Lease::Lease(Lease&& other) noexcept
    : owner(std::exchange(other.owner, nullptr)),
      priority(other.priority),
      preemptionSource(std::move(other.preemptionSource))
{
}

InferenceScheduler::Lease& InferenceScheduler::Lease::operator=(Lease&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        owner = std::exchange(other.owner, nullptr);
        priority = other.priority;
        preemptionSource = std::move(other.preemptionSource);
    }
    return *this;
}

InferenceScheduler::Lease::operator bool() const
{
    return owner != nullptr;
}

std::stop_token InferenceScheduler::Lease::PreemptionToken() const
{
    return preemptionSource ? preemptionSource->get_token() : std::stop_token{};
}

void InferenceScheduler::Lease::Reset()
{
    if (owner != nullptr)
    {
        owner->Release(priority);
        owner = nullptr;
    }
    preemptionSource.reset();
}

void InferenceScheduler::SetCapacity(const int slots)
{
    {
        std::lock_guard lock(mutex);
        capacity = std::clamp(slots, 1, 16);
    }
    available.notify_all();
}

InferenceScheduler::Lease InferenceScheduler::Acquire(
    const InferencePriority priority,
    const std::stop_token stopToken)
{
    std::unique_lock lock(mutex);
    int& waiting = priority == InferencePriority::Interactive
        ? waitingInteractive
        : waitingBackground;
    ++waiting;
    if (priority == InferencePriority::Interactive && active >= capacity &&
        activeBackground > 0)
    {
        // A background classification is expendable; making the person wait behind it is
        // not. The request still owns cleanup and releases its lease after client.stop()
        // returns, so capacity accounting remains exact.
        backgroundCancellations.erase(
            std::remove_if(
                backgroundCancellations.begin(),
                backgroundCancellations.end(),
                [](const std::weak_ptr<std::stop_source>& source)
                {
                    const std::shared_ptr<std::stop_source> activeSource = source.lock();
                    if (!activeSource)
                    {
                        return true;
                    }
                    activeSource->request_stop();
                    return false;
                }),
            backgroundCancellations.end());
    }
    const auto canEnter = [&]()
    {
        return active < capacity &&
            (priority == InferencePriority::Interactive || waitingInteractive == 0);
    };

    bool admitted = false;
    if (stopToken.stop_possible())
    {
        admitted = available.wait(lock, stopToken, canEnter);
    }
    else
    {
        available.wait(lock, canEnter);
        admitted = true;
    }
    --waiting;
    if (!admitted || stopToken.stop_requested())
    {
        available.notify_all();
        return {};
    }

    ++active;
    if (priority == InferencePriority::Background)
    {
        ++activeBackground;
        auto preemptionSource = std::make_shared<std::stop_source>();
        backgroundCancellations.push_back(preemptionSource);
        return Lease(this, priority, std::move(preemptionSource));
    }
    return Lease(this, priority);
}

InferenceSchedulerSnapshot InferenceScheduler::Snapshot() const
{
    std::lock_guard lock(mutex);
    return {capacity, active, activeBackground, waitingInteractive, waitingBackground};
}

void InferenceScheduler::Release(const InferencePriority priority)
{
    {
        std::lock_guard lock(mutex);
        active = std::max(0, active - 1);
        if (priority == InferencePriority::Background)
        {
            activeBackground = std::max(0, activeBackground - 1);
            backgroundCancellations.erase(
                std::remove_if(
                    backgroundCancellations.begin(),
                    backgroundCancellations.end(),
                    [](const std::weak_ptr<std::stop_source>& source)
                    {
                        return source.expired();
                    }),
                backgroundCancellations.end());
        }
    }
    available.notify_all();
}

} // namespace revia::llm
