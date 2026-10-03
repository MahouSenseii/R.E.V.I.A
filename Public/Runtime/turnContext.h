#pragma once

#include "Runtime/runtimeEvents.h"
#include "Runtime/sessionResult.h"

#include <cstdint>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace revia::runtime
{

// Immutable TurnContext carries no session pointer, callbacks or service bag.
// Subsystems return typed TurnOutcome effects; the session alone applies state and publication changes.

// One thing that happened during a turn, in the vocabulary the session already publishes.
struct TurnEvent
{
    enum class Kind
    {
        // The runtime state moved. The session decides whether to honour it: a
        // subsystem saying "Idle" while another turn is running is describing its own
        // work, not the session's.
        State,
        // Progress on a named component, for the activity panel.
        Component,
        // Something for the canvas or another surface, already shaped as a runtime
        // event. Carried whole because these are published verbatim.
        Runtime
    };

    Kind kind = Kind::Component;

    // Kind::State
    RuntimeState state = RuntimeState::Idle;
    std::string activity;

    // Kind::Component
    std::string component;
    std::string phase;
    std::string message;
    double elapsedMilliseconds = -1.0;
    std::string resource;

    // Kind::Runtime
    RuntimeEvent runtimeEvent;

    [[nodiscard]] static TurnEvent Moved(RuntimeState state, std::string activity = {})
    {
        TurnEvent event;
        event.kind = Kind::State;
        event.state = state;
        event.activity = std::move(activity);
        return event;
    }

    [[nodiscard]] static TurnEvent Progress(
        std::string component, std::string phase, std::string message, const double elapsedMilliseconds = -1.0, std::string resource = {})
    {
        TurnEvent event;
        event.kind = Kind::Component;
        event.component = std::move(component);
        event.phase = std::move(phase);
        event.message = std::move(message);
        event.elapsedMilliseconds = elapsedMilliseconds;
        event.resource = std::move(resource);
        return event;
    }

    [[nodiscard]] static TurnEvent Published(RuntimeEvent published)
    {
        TurnEvent event;
        event.kind = Kind::Runtime;
        event.runtimeEvent = std::move(published);
        return event;
    }
};

// What one unit of work is allowed to know about the session it runs inside.
//
// Immutable by construction: every field is const, so a subsystem physically cannot use
// this as a channel back. Deliberately small -- it holds facts about *this turn*, never
// services, never handles, and never the session.
struct TurnContext
{
    // Exactly what was asked, already accepted and trimmed by the session.
    const std::string request;
    // The operation's token. A subsystem that does not check it is a subsystem that
    // cannot be stopped, which is why it is here rather than optional.
    const std::stop_token cancellation;
    // Which turn this is, for joining events to a reply.
    const std::uint64_t turnId = 0;
    // Whether the current output channel speaks. Read-only: a subsystem may shape its
    // reply for a voice, and may not change where it goes.
    const bool speaking = false;
    const RuntimeStamp stamp;

    [[nodiscard]] bool Cancelled() const
    {
        return cancellation.stop_possible() && cancellation.stop_requested();
    }
};

// The reply and everything that happened on the way to it.
struct TurnOutcome
{
    SessionResult result;
    std::vector<TurnEvent> events;

    TurnOutcome& Then(TurnEvent event)
    {
        events.push_back(std::move(event));
        return *this;
    }
};

} // namespace revia::runtime
