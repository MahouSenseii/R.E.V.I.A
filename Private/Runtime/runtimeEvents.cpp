#include "Runtime/runtimeEvents.h"

#include <utility>

namespace revia::runtime
{

std::string ToString(const RuntimeState state)
{
    switch (state)
    {
    case RuntimeState::Offline:
        return "Offline";
    case RuntimeState::Starting:
        return "Starting";
    case RuntimeState::Idle:
        return "Idle";
    case RuntimeState::Thinking:
        return "Thinking";
    case RuntimeState::Responding:
        return "Responding";
    case RuntimeState::Remembering:
        return "Remembering";
    case RuntimeState::Acting:
        return "Acting";
    case RuntimeState::WaitingForConfirmation:
        return "Waiting for confirmation";
    case RuntimeState::Blocked:
        return "Blocked";
    case RuntimeState::Error:
        return "Error";
    case RuntimeState::Stopping:
        return "Stopping";
    default:
        return "Unknown";
    }
}

RuntimeEventBus::SubscriptionId RuntimeEventBus::Subscribe(Handler handler)
{
    if (!handler)
    {
        return 0;
    }

    std::lock_guard lock(mutex);
    const SubscriptionId id = nextId++;
    handlers.emplace(id, std::move(handler));
    return id;
}

void RuntimeEventBus::Unsubscribe(const SubscriptionId id)
{
    std::lock_guard lock(mutex);
    handlers.erase(id);
}

void RuntimeEventBus::BindOrigin(RuntimeStamp inputOrigin, std::function<bool(const RuntimeStamp&)> inputAdmission)
{
    std::lock_guard lock(mutex);
    origin = std::move(inputOrigin);
    admission = std::move(inputAdmission);
}

void RuntimeEventBus::Publish(RuntimeEvent event) const
{
    std::vector<Handler> snapshot;
    std::function<bool(const RuntimeStamp&)> currentAdmission;
    {
        std::lock_guard lock(mutex);
        if (!origin.companionId.empty())
        {
            if (event.stamp.companionId.empty())
                event.stamp = origin;
            if (!event.stamp.SameSession(origin))
                return;
        }
        currentAdmission = admission;
        snapshot.reserve(handlers.size());
        for (const auto& [id, handler] : handlers)
        {
            (void)id;
            snapshot.push_back(handler);
        }
    }

    if (currentAdmission && !currentAdmission(event.stamp))
        return;

    for (const Handler& handler : snapshot)
    {
        try
        {
            handler(event);
        }
        catch (...)
        {
            // One presentation listener must not break the runtime or other listeners.
        }
    }
}

} // namespace revia::runtime
