#include "testSupport.h"

#include "Presentation/avatarState.h"
#include "Presentation/debugPresentationSink.h"
#include "Presentation/presentationBus.h"
#include "Runtime/runtimeEvents.h"

#include <iostream>
#include <memory>
#include <string>

namespace
{

using namespace revia::presentation;
using revia::tests::Check;

PresentationEvent Event(const PresentationEventKind kind, const std::string& subject = {})
{
    PresentationEvent event;
    event.kind = kind;
    event.subject = subject;
    return event;
}

void TestMoodChangeReachesPresentation()
{
    PresentationBus bus;
    auto controller = std::make_shared<PresentationController>();
    auto debug = std::make_shared<DebugPresentationSink>();
    bus.Add(controller);
    bus.Add(debug);

    PresentationEvent sad = Event(PresentationEventKind::EmotionChanged);
    sad.affect = revia::runtime::AffectState::Melancholy;
    sad.affectIntensity = 0.8F;
    bus.Publish(sad);

    const AvatarState state = controller->Snapshot();
    Check(state.expression == revia::runtime::AffectState::Melancholy,
        "The expression did not follow the emotion.");
    Check(state.moodValence < 0.0F, "A low mood did not read as negative valence.");
    Check(state.energy < 0.5F, "A low mood did not lower energy.");
    Check(state.idle == IdleMotion::Subdued,
        "A low mood did not soften the idle motion.");
    Check(debug->Lines().size() == 1, "The debug sink saw nothing.");

    // Curiosity is the other direction, and it must reach the gaze.
    PresentationEvent curious = Event(PresentationEventKind::CuriosityRaised, "the new build");
    curious.affectIntensity = 0.9F;
    bus.Publish(curious);
    const AvatarState attentive = controller->Snapshot();
    Check(attentive.idle == IdleMotion::Attentive && attentive.gaze == GazeTarget::User,
        "High curiosity did not produce an attentive, user-facing state.");
}

void TestSpeechUpdatesAvatarState()
{
    PresentationBus bus;
    auto controller = std::make_shared<PresentationController>();
    bus.Add(controller);

    bus.Publish(Event(PresentationEventKind::StartedThinking));
    Check(controller->Snapshot().thinking, "Thinking did not become visible.");

    bus.Publish(Event(PresentationEventKind::StartedSpeaking));
    const AvatarState speaking = controller->Snapshot();
    Check(speaking.speaking && speaking.lipSync, "Speaking did not drive the mouth.");
    Check(!speaking.thinking, "She was still shown thinking while speaking.");

    bus.Publish(Event(PresentationEventKind::StoppedSpeaking));
    const AvatarState quiet = controller->Snapshot();
    Check(!quiet.speaking && !quiet.lipSync, "The mouth kept moving after speech ended.");
}

void TestSingingEmitsPerformanceState()
{
    PresentationBus bus;
    auto controller = std::make_shared<PresentationController>();
    bus.Add(controller);

    bus.Publish(Event(PresentationEventKind::StartedSinging, "Wide Open"));
    const AvatarState intro = controller->Snapshot();
    Check(intro.singing && intro.performanceMode, "Singing did not enter performance mode.");
    // The intro has no vocal yet, and a mouth moving through it is the tell that the rig
    // is being driven by the track instead of the voice.
    Check(!intro.lipSync, "The mouth opened before the vocal line started.");
    Check(intro.activity == "Wide Open", "The song title did not reach visible state.");

    bus.Publish(Event(PresentationEventKind::StoppedSinging));
    const AvatarState after = controller->Snapshot();
    Check(!after.singing && !after.performanceMode && !after.lipSync,
        "Performance state survived the end of the song.");
}

void TestVocalEventsDriveTheMouth()
{
    PresentationBus bus;
    auto controller = std::make_shared<PresentationController>();
    bus.Add(controller);

    bus.Publish(Event(PresentationEventKind::StartedSinging, "Wide Open"));
    bus.Publish(Event(PresentationEventKind::VocalStarted));
    Check(controller->Snapshot().lipSync, "The vocal line did not open the mouth.");
    bus.Publish(Event(PresentationEventKind::VocalEnded));
    Check(!controller->Snapshot().lipSync, "The mouth stayed open through an instrumental.");

    // And a spoken line ending mid-song must not close a mouth the song owns.
    bus.Publish(Event(PresentationEventKind::VocalStarted));
    bus.Publish(Event(PresentationEventKind::StoppedSpeaking));
    Check(controller->Snapshot().lipSync,
        "A speech event closed a mouth that belonged to the song.");
}

void TestHiddenReasoningNeverBecomesPresentation()
{
    // The rule that matters most here. SelfInquiry is Revia's private reasoning, and the
    // translation has to drop it rather than relabel it -- a renderer must be able to
    // show that she is thinking without being able to show what.
    revia::runtime::RuntimeEvent inquiry;
    inquiry.kind = revia::runtime::RuntimeEventKind::SelfInquiry;
    inquiry.message = "Is he asking because he is upset with me?";
    inquiry.detail = "He used a shorter sentence than usual.";
    Check(!TranslateRuntimeEvent(inquiry).has_value(),
        "Private reasoning was translated into a presentation event.");

    // Nor may evidence text ride along on an event that does translate.
    revia::runtime::RuntimeEvent thinking;
    thinking.kind = revia::runtime::RuntimeEventKind::StateChanged;
    thinking.state = revia::runtime::RuntimeState::Thinking;
    thinking.message = "Weighing whether to mention the failed build";
    thinking.detail = "He seemed tired, and this can wait until tomorrow.";
    const auto translated = TranslateRuntimeEvent(thinking);
    Check(translated.has_value(), "A visible state change did not translate.");
    Check(translated->kind == PresentationEventKind::StartedThinking,
        "Thinking did not translate to a thinking state.");
    Check(translated->subject.empty(),
        "Reasoning text rode into presentation on the subject field.");

    // And the bus caps whatever a publisher does put there, so no future call site can
    // pour a paragraph through.
    PresentationBus bus;
    auto debug = std::make_shared<DebugPresentationSink>();
    bus.Add(debug);
    bus.Publish(Event(PresentationEventKind::StartedResearching, std::string(400, 'x')));
    Check(debug->Lines().size() == 1, "The event did not reach the sink.");
    Check(debug->Lines().front().size() < 200,
        "An oversized subject reached a sink uncapped.");
}

void TestSinksAreIndependentAndOrdered()
{
    PresentationBus bus;
    auto controller = std::make_shared<PresentationController>();
    auto first = std::make_shared<DebugPresentationSink>();
    auto second = std::make_shared<DebugPresentationSink>();
    bus.Add(controller);
    const PresentationBus::SubscriptionId secondId = bus.Add(second);
    bus.Add(first);

    bus.Publish(Event(PresentationEventKind::StartedListening));
    bus.Publish(Event(PresentationEventKind::StoppedListening));
    Check(first->Lines().size() == 2 && second->Lines().size() == 2,
        "Both sinks did not receive both events.");
    Check(bus.Published() == 2, "The bus miscounted what it published.");

    // Removing a renderer is not supposed to disturb the rest of the world.
    bus.Remove(secondId);
    bus.Publish(Event(PresentationEventKind::BecameIdle));
    Check(second->Lines().size() == 2, "A removed sink kept receiving events.");
    Check(first->Lines().size() == 3, "Removing one sink stopped another.");
    Check(bus.SinkNames().size() == 2, "The bus lost track of its sinks.");

    // The debug sink is the proof that visible state is renderable from events alone.
    const std::string described = DebugPresentationSink::Describe(controller->Snapshot());
    Check(!described.empty(), "Visible state could not be described.");
}

void TestPerformanceRuntimeEventsTranslate()
{
    // The song events already exist on the runtime bus. This is the check that they can
    // reach a renderer without the renderer knowing about PerformanceRuntime.
    revia::runtime::RuntimeEvent started;
    started.kind = revia::runtime::RuntimeEventKind::Performance;
    started.phase = "SongStarted";
    started.message = "Wide Open";
    started.detail = "first karaoke line";
    const auto translated = TranslateRuntimeEvent(started);
    Check(translated.has_value() &&
        translated->kind == PresentationEventKind::StartedSinging,
        "A song starting did not translate.");
    Check(translated->subject == "Wide Open", "The song title did not carry over.");

    revia::runtime::RuntimeEvent vocal;
    vocal.kind = revia::runtime::RuntimeEventKind::Performance;
    vocal.phase = "VocalStarted";
    vocal.detail = "the lyrics of the line";
    const auto vocalEvent = TranslateRuntimeEvent(vocal);
    Check(vocalEvent.has_value() && vocalEvent->kind == PresentationEventKind::VocalStarted,
        "A vocal cue did not translate.");
    // Lyrics are content, not state.
    Check(vocalEvent->subject.empty(), "Karaoke lyrics rode into a presentation event.");
}

} // namespace

void RunPresentationTests()
{
    TestMoodChangeReachesPresentation();
    TestSpeechUpdatesAvatarState();
    TestSingingEmitsPerformanceState();
    TestVocalEventsDriveTheMouth();
    TestHiddenReasoningNeverBecomesPresentation();
    TestSinksAreIndependentAndOrdered();
    TestPerformanceRuntimeEventsTranslate();
    std::cout << "Presentation tests passed: a renderer could exist, and could not read "
                 "her mind.\n";
}
