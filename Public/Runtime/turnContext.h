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

// The boundary a subsystem does one turn's work across.
//
// The three remaining responsibilities inside ReviaSession -- drafting and pictures,
// initiative and curiosity, the screen-awareness capture body -- were not left there
// because nobody looked at them. They were left there because every one of them is
// interleaved with the session's own lifecycle: they set the runtime state, they publish
// component progress, they read the foreground lock and the busy flag, and they do all of
// that by calling private members of a nine-thousand-line class. Moving such a thing
// behind a callback produces a forwarding layer -- the same monolith, in more files, with
// a vtable in the middle -- which is exactly what the brief says does not count.
//
// What was missing was a way for a subsystem to *do* a unit of work without being able to
// reach back. This is that boundary, and it is two halves:
//
//   in   TurnContext -- an immutable description of the one turn. A subsystem reads it
//        and cannot change it, and it carries no pointer to the session.
//   out  TurnOutcome -- the reply, plus the state changes and progress the work produced,
//        as *data*. The session applies them.
//
// The inversion is the whole point. A subsystem that called SetState could drive the
// session's state machine from the outside, and every caller had to be trusted not to.
// A subsystem that returns a list of typed events can only describe what happened; what
// that means for the session stays the session's decision. The test for whether an
// extraction is real is now mechanical: if the moved code still needs the session, it did
// not move.
//
// What this deliberately is not: a service bag. It carries no collaborators. A subsystem
// gets its dependencies through its own constructor, named one by one, because that is
// what makes the dependency list readable and finite. A struct that grew a `session*`, or
// a set of std::functions mirroring the session's private methods, would be this same
// mistake wearing a new name.

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
        std::string component,
        std::string phase,
        std::string message,
        const double elapsedMilliseconds = -1.0,
        std::string resource = {})
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
