#include "testSupport.h"

#include "Runtime/turnContext.h"

#include <iostream>
#include <string>

namespace
{

using namespace revia::runtime;
using revia::tests::Check;

// The boundary the remaining extractions were waiting for.
//
// These do not test drafting or diagrams -- those are covered where they always were,
// through the session. They test the thing that made moving drafting and diagrams
// possible at all: that a subsystem can do a turn's work with no way to reach back into
// the session, and that what it did arrives as data the session applies.
//
// Worth testing directly because the property is easy to lose. The moment someone adds a
// `ReviaSession&` to a constructor or a `std::function<void(RuntimeState)>` to a context,
// the extraction silently becomes a forwarding layer again and every other test still
// passes.

// A turn context cannot be used as a channel back.
//
// This is a compile-time property and it is asserted as one. Every field is const, so
// there is no assignment a subsystem could make; the static_asserts below fail to compile
// if that is ever relaxed, which is the only kind of check that actually holds.
void TestATurnContextCannotBeWrittenTo()
{
    static_assert(!std::is_copy_assignable_v<TurnContext>,
        "TurnContext became assignable, so a subsystem can now overwrite the turn it was "
        "given. Every field must stay const.");
    static_assert(std::is_const_v<decltype(TurnContext::request)>,
        "The request stopped being const.");
    static_assert(std::is_const_v<decltype(TurnContext::speaking)>,
        "The output channel stopped being const, so a subsystem could redirect where its "
        "reply goes -- which is the session's decision, not its own.");

    std::stop_source source;
    const TurnContext context{"draft something", source.get_token(), 7, false};
    Check(!context.Cancelled(), "A fresh turn reported itself already cancelled.");
    source.request_stop();
    Check(context.Cancelled(),
        "A stopped turn did not report itself cancelled, so a subsystem checking this "
        "would keep working after the user asked it to stop.");
}

// The events a subsystem produces are data, and stay data until someone applies them.
void TestWorkDescribesItselfRatherThanPerformingItself()
{
    TurnOutcome outcome;
    outcome.result.succeeded = true;
    outcome.result.text = "Drafted three blocks.";
    outcome.Then(TurnEvent::Moved(RuntimeState::Thinking, "Drafting."))
        .Then(TurnEvent::Progress("Document", "Drafting", "Composing new material."))
        .Then(TurnEvent::Moved(RuntimeState::Idle));

    Check(outcome.events.size() == 3,
        "The events were not all recorded, so applying them would replay a different "
        "turn than the one that happened.");
    Check(outcome.events.front().kind == TurnEvent::Kind::State &&
            outcome.events.front().state == RuntimeState::Thinking,
        "A state change was not carried as a state change.");
    Check(outcome.events[1].kind == TurnEvent::Kind::Component &&
            outcome.events[1].component == "Document",
        "Component progress was not carried as component progress.");
    Check(outcome.events.back().state == RuntimeState::Idle,
        "The final state was lost.");

    // The point of the whole arrangement: this is a list, not a set of calls that have
    // already happened. Nothing above touched a session, a state machine or an event
    // bus, and the order is preserved so the session can apply them faithfully.
    Check(outcome.result.text == "Drafted three blocks.",
        "The reply did not survive alongside the events.");
}

// Order matters and is preserved. A session that applied these out of order would show
// "Idle" before the work it describes.
void TestEventsKeepTheOrderTheyHappenedIn()
{
    TurnOutcome outcome;
    for (int index = 0; index < 5; ++index)
    {
        outcome.Then(TurnEvent::Progress("Image", "Step", std::to_string(index)));
    }
    for (int index = 0; index < 5; ++index)
    {
        Check(outcome.events[static_cast<std::size_t>(index)].message ==
                std::to_string(index),
            "The events were reordered, so a replay would not be the turn that ran.");
    }
}

// A runtime event -- a canvas publication, say -- is carried whole rather than
// reconstructed, because reconstructing it is where fields get dropped.
void TestACanvasPublicationSurvivesTheBoundary()
{
    RuntimeEvent published;
    published.kind = RuntimeEventKind::Diagram;
    published.component = "Canvas";
    published.phase = "Image";
    published.message = "a picture";
    published.resource = "C:/pictures/one.png";

    TurnOutcome outcome;
    outcome.Then(TurnEvent::Published(published));

    Check(outcome.events.front().kind == TurnEvent::Kind::Runtime,
        "A canvas publication was flattened into something else.");
    Check(outcome.events.front().runtimeEvent.resource == "C:/pictures/one.png" &&
            outcome.events.front().runtimeEvent.phase == "Image",
        "The publication lost fields crossing the boundary, which is how a canvas ends "
        "up told to parse markup when it was handed a file.");
}

} // namespace

void RunTurnBoundaryTests()
{
    TestATurnContextCannotBeWrittenTo();
    TestWorkDescribesItselfRatherThanPerformingItself();
    TestEventsKeepTheOrderTheyHappenedIn();
    TestACanvasPublicationSurvivesTheBoundary();

    std::cout << "A subsystem is handed an immutable turn and hands back what it did; the "
                 "session is the\nonly thing that turns either of those into an effect.\n";
}
