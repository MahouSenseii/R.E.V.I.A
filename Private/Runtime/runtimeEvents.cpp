#include "Runtime/runtimeEvents.h"

#include <utility>

namespace revia::runtime
{

std::string ToString(const RuntimeState state)
{
    switch (state)
    {
        case RuntimeState::Offline: return "Offline";
        case RuntimeState::Starting: return "Starting";
        case RuntimeState::Idle: return "Idle";
        case RuntimeState::Thinking: return "Thinking";
        case RuntimeState::Responding: return "Responding";
        case RuntimeState::Remembering: return "Remembering";
        case RuntimeState::Acting: return "Acting";
        case RuntimeState::WaitingForConfirmation: return "Waiting for confirmation";
        case RuntimeState::Blocked: return "Blocked";
        case RuntimeState::Error: return "Error";
        case RuntimeState::Stopping: return "Stopping";
        default: return "Unknown";
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

void RuntimeEventBus::Publish(RuntimeEvent event) const
{
    std::vector<Handler> snapshot;
    {
        std::lock_guard lock(mutex);
        snapshot.reserve(handlers.size());
        for (const auto& [id, handler] : handlers)
        {
            (void)id;
            snapshot.push_back(handler);
        }
    }

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
