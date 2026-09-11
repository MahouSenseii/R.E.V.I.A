#include "testSupport.h"

#include "Speech/speechCoordinator.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace revia::speech;
using revia::tests::Check;

// A stand-in for the audio channel.
//
// The point of testing the coordinator rather than SpeechService is that arbitration is
// a decision, and a decision can be checked without a GPU, a voice, or a sound card.
// This records what the coordinator asked the world to do, and -- importantly -- keeps
// its own count of how many things are audible at once, so "they overlapped" is
// observable rather than inferred.
struct Channel
{
    std::vector<std::uint64_t> started;
    std::vector<std::string> stoppedSongs;
    int stopSpeechCalls = 0;
    int audible = 0;
    int mostAudibleAtOnce = 0;

    SpeechChannel Wire()
    {
        SpeechChannel wiring;
        wiring.speak = [this](const SpeechIntent&, const std::uint64_t id)
        {
            started.push_back(id);
            ++audible;
            mostAudibleAtOnce = std::max(mostAudibleAtOnce, audible);
        };
        wiring.stopSpeech = [this]()
        {
            ++stopSpeechCalls;
            if (audible > 0) --audible;
        };
        wiring.stopSong = [this](const std::string& reason)
        {
            stoppedSongs.push_back(reason);
        };
        return wiring;
    }

    // The coordinator reports completion, and so does the real speech backend.
    void Finished(SpeechCoordinator& coordinator, const std::uint64_t id)
    {
        if (audible > 0) --audible;
        coordinator.NotePlaybackFinished(id);
    }
};

SpeechIntent Intent(
    const SpeechOwner owner,
    const SpeechBehavior behavior,
    const std::string& text = "something")
{
    SpeechIntent intent;
    intent.owner = owner;
    intent.behavior = behavior;
    intent.text = text;
    return intent;
}

void TestUserSpeechInterruptsIdleChatter()
{
    Channel channel;
    SpeechCoordinator coordinator;
    coordinator.SetChannel(channel.Wire());

    const SpeechSubmission chatter =
        coordinator.Submit(Intent(SpeechOwner::Autonomy, SpeechBehavior::Queue));
    Check(chatter.state == SpeechIntentState::Speaking,
        "Autonomous chatter did not take a free channel.");

    const SpeechSubmission reply =
        coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Interrupt));
    Check(reply.accepted && reply.state == SpeechIntentState::Speaking,
        "Answering the user did not take the floor from idle chatter.");
    Check(channel.stopSpeechCalls == 1, "The chatter was not actually stopped.");
    Check(coordinator.Status().activeOwner == SpeechOwner::Conversation,
        "The channel did not end up owned by the conversation.");
}

void TestChatterDoesNotInterruptConversation()
{
    Channel channel;
    SpeechCoordinator coordinator;
    coordinator.SetChannel(channel.Wire());

    coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Queue));
    // The behaviour is a request, not an entitlement. Asking to interrupt is exactly how
    // a companion becomes intolerable, so asking is not enough.
    const SpeechSubmission chatter =
        coordinator.Submit(Intent(SpeechOwner::Autonomy, SpeechBehavior::Interrupt));
    Check(chatter.state != SpeechIntentState::Speaking,
        "An autonomous remark interrupted a reply to the user.");
    Check(channel.stopSpeechCalls == 0, "A reply to the user was stopped for chatter.");
    Check(coordinator.Status().activeOwner == SpeechOwner::Conversation,
        "The conversation lost the channel.");
}

void TestSystemSpeechInterruptsNormalSpeech()
{
    Channel channel;
    SpeechCoordinator coordinator;
    coordinator.SetChannel(channel.Wire());

    coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Queue));
    const SpeechSubmission alarm =
        coordinator.Submit(Intent(SpeechOwner::System, SpeechBehavior::Interrupt));
    Check(alarm.accepted && alarm.state == SpeechIntentState::Speaking,
        "System-critical speech could not interrupt an ordinary reply.");
    Check(coordinator.Status().activeOwner == SpeechOwner::System,
        "The system did not end up owning the channel.");
}

void TestQueuePreservesOrder()
{
    Channel channel;
    SpeechCoordinator coordinator;
    coordinator.SetChannel(channel.Wire());

    const SpeechSubmission first =
        coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Queue, "one"));
    const SpeechSubmission second =
        coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Queue, "two"));
    const SpeechSubmission third =
        coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Queue, "three"));
    Check(second.state == SpeechIntentState::Queued &&
        third.state == SpeechIntentState::Queued, "Later sentences did not queue.");

    channel.Finished(coordinator, first.id);
    channel.Finished(coordinator, second.id);
    channel.Finished(coordinator, third.id);

    const std::vector<std::uint64_t> expected{first.id, second.id, third.id};
    Check(channel.started == expected,
        "Sentences of one reply were spoken out of order.");
}

void TestReplaceRemovesObsoleteSpeech()
{
    Channel channel;
    SpeechCoordinator coordinator;
    coordinator.SetChannel(channel.Wire());

    const SpeechSubmission holding =
        coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Queue));

    SpeechIntent stale = Intent(SpeechOwner::Skill, SpeechBehavior::Queue, "42 percent");
    stale.replaceKey = "download";
    const SpeechSubmission staleSubmission = coordinator.Submit(stale);
    Check(staleSubmission.state == SpeechIntentState::Queued, "The first status did not queue.");

    SpeechIntent fresh = Intent(SpeechOwner::Skill, SpeechBehavior::Replace, "91 percent");
    fresh.replaceKey = "download";
    const SpeechSubmission freshSubmission = coordinator.Submit(fresh);
    Check(freshSubmission.accepted, "The newer status was not accepted.");

    channel.Finished(coordinator, holding.id);
    // The obsolete number must never have been said. A progress report that announces
    // 42 percent after 91 percent is worse than no progress report.
    const bool spokeStale = std::find(channel.started.begin(), channel.started.end(),
        staleSubmission.id) != channel.started.end();
    Check(!spokeStale, "An obsolete status line was spoken after being replaced.");
    const bool spokeFresh = std::find(channel.started.begin(), channel.started.end(),
        freshSubmission.id) != channel.started.end();
    Check(spokeFresh, "The replacement was never spoken.");

    // A different key from the same owner is a different statement and survives.
    SpeechIntent other = Intent(SpeechOwner::Skill, SpeechBehavior::Queue, "build finished");
    other.replaceKey = "build";
    const SpeechSubmission otherSubmission = coordinator.Submit(other);
    SpeechIntent newer = Intent(SpeechOwner::Skill, SpeechBehavior::Replace, "99 percent");
    newer.replaceKey = "download";
    coordinator.Submit(newer);
    Check(otherSubmission.state == SpeechIntentState::Queued,
        "An unrelated statement was queued incorrectly.");
    bool buildStillPending = false;
    for (const TrackedIntent& step : coordinator.History())
    {
        if (step.id == otherSubmission.id && step.state == SpeechIntentState::Replaced)
        {
            buildStillPending = true;
        }
    }
    Check(!buildStillPending,
        "Replacing one statement discarded an unrelated one from the same owner.");
}

void TestCancellationPreventsLaterPlayback()
{
    Channel channel;
    SpeechCoordinator coordinator;
    coordinator.SetChannel(channel.Wire());

    const SpeechSubmission holding =
        coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Queue));
    const SpeechSubmission doomed =
        coordinator.Submit(Intent(SpeechOwner::Research, SpeechBehavior::Queue));
    Check(doomed.state == SpeechIntentState::Queued, "The second intent did not queue.");

    coordinator.Cancel(doomed.id, "the goal it belonged to was abandoned");
    channel.Finished(coordinator, holding.id);

    const bool spoke = std::find(channel.started.begin(), channel.started.end(), doomed.id)
        != channel.started.end();
    Check(!spoke, "A canceled intent was spoken once the channel freed up.");
}

void TestCanceledIntentCannotRestart()
{
    Channel channel;
    SpeechCoordinator coordinator;
    coordinator.SetChannel(channel.Wire());

    const SpeechSubmission speaking =
        coordinator.Submit(Intent(SpeechOwner::Autonomy, SpeechBehavior::Queue));
    coordinator.Cancel(speaking.id, "withdrawn");
    Check(!coordinator.Status().speaking, "A canceled intent kept the channel.");

    // Every later report about it is a chance to resurrect it, and none of them may.
    coordinator.NotePlaybackFinished(speaking.id);
    coordinator.NotePerformanceEnded();
    coordinator.NoteUserSpoke();

    int timesStarted = 0;
    for (const std::uint64_t id : channel.started)
    {
        if (id == speaking.id) ++timesStarted;
    }
    Check(timesStarted == 1, "A canceled intent started more than once.");
    Check(!coordinator.Status().speaking, "A canceled intent came back.");
}

void TestSingingOwnershipFollowsSettings()
{
    // Songs are set to keep playing: the reply is not spoken over the music.
    {
        Channel channel;
        SpeechCoordinator coordinator;
        coordinator.SetChannel(channel.Wire());
        SongPolicy policy;
        policy.interruptSongToSpeak = false;
        coordinator.SetSongPolicy(policy);
        coordinator.NotePerformanceStarted("wide-open");

        const SpeechSubmission reply =
            coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Interrupt));
        Check(reply.state == SpeechIntentState::Ignored,
            "A reply talked over a song the user asked to keep playing.");
        Check(channel.stoppedSongs.empty(), "The song was stopped against the setting.");
    }

    // Songs yield: answering the user stops the music, idle chatter does not.
    {
        Channel channel;
        SpeechCoordinator coordinator;
        coordinator.SetChannel(channel.Wire());
        SongPolicy policy;
        policy.interruptSongToSpeak = true;
        coordinator.SetSongPolicy(policy);
        coordinator.NotePerformanceStarted("wide-open");

        const SpeechSubmission chatter =
            coordinator.Submit(Intent(SpeechOwner::Autonomy, SpeechBehavior::Interrupt));
        Check(chatter.state == SpeechIntentState::Ignored,
            "Idle chatter stopped a song.");
        Check(channel.stoppedSongs.empty(), "A song was stopped for idle chatter.");

        const SpeechSubmission reply =
            coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Interrupt));
        Check(reply.state == SpeechIntentState::Speaking,
            "Answering the user did not stop a song that was set to yield.");
        Check(channel.stoppedSongs.size() == 1, "The song was not stopped.");
        Check(!coordinator.Status().performing, "The coordinator still thinks she is singing.");
    }
}

void TestBargeInStopsCurrentSpeech()
{
    Channel channel;
    SpeechCoordinator coordinator;
    coordinator.SetChannel(channel.Wire());

    const SpeechSubmission talking =
        coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Queue));
    const SpeechSubmission queuedChatter =
        coordinator.Submit(Intent(SpeechOwner::Autonomy, SpeechBehavior::Queue));

    coordinator.NoteUserSpoke();
    Check(channel.stopSpeechCalls == 1, "Barge-in did not stop the speech.");
    Check(!coordinator.Status().speaking, "The channel stayed busy after barge-in.");
    Check(coordinator.Status().queueDepth == 0,
        "Autonomous speech survived barge-in and would have followed the user's sentence.");

    // Nothing resumes afterwards. Finishing the interrupted utterance is the exact
    // report that would restart a queue that must stay empty.
    coordinator.NotePlaybackFinished(talking.id);
    const bool chatterSpoke = std::find(channel.started.begin(), channel.started.end(),
        queuedChatter.id) != channel.started.end();
    Check(!chatterSpoke, "Chatter dropped by barge-in was spoken anyway.");
}

void TestOwnersDoNotOverlap()
{
    Channel channel;
    SpeechCoordinator coordinator;
    coordinator.SetChannel(channel.Wire());

    // Every owner asks for the channel at once, in the least convenient order, and each
    // one asks to interrupt.
    const SpeechOwner owners[]{
        SpeechOwner::Autonomy, SpeechOwner::Conversation, SpeechOwner::Game,
        SpeechOwner::Skill, SpeechOwner::System, SpeechOwner::Research};
    std::vector<SpeechSubmission> submissions;
    for (const SpeechOwner owner : owners)
    {
        submissions.push_back(coordinator.Submit(Intent(owner, SpeechBehavior::Interrupt)));
    }

    Check(channel.mostAudibleAtOnce == 1,
        "Two owners were audible at the same time (" +
        std::to_string(channel.mostAudibleAtOnce) + ").");

    // Drain, reporting completion for whatever is speaking, and keep checking.
    for (int step = 0; step < 16; ++step)
    {
        const SpeechChannelStatus status = coordinator.Status();
        if (!status.speaking) break;
        channel.Finished(coordinator, status.activeId);
    }
    Check(channel.mostAudibleAtOnce == 1,
        "Draining the queue produced overlapping playback.");
    Check(coordinator.Status().queueDepth == 0, "The queue did not drain.");
}

void TestSpeechTheCoordinatorDidNotStartStillHoldsTheFloor()
{
    // The reply path streams sentences straight into SpeechService, because serialising
    // that through this queue would make every answer slower. So the coordinator asks
    // whether the throat is busy instead of assuming it would know.
    Channel channel;
    SpeechCoordinator coordinator;
    coordinator.SetChannel(channel.Wire());

    bool replyPlaying = true;
    coordinator.SetBusyProbe([&]() { return replyPlaying; });

    // A remark that is worth making now or not at all is simply dropped.
    const SpeechSubmission chatter =
        coordinator.Submit(Intent(SpeechOwner::Autonomy, SpeechBehavior::IgnoreIfBusy));
    Check(chatter.state == SpeechIntentState::Ignored,
        "An autonomous remark talked over a reply that had not gone through the "
        "coordinator.");
    Check(channel.started.empty(), "Something was spoken over an active reply.");

    // One that asks to interrupt does not get to, either. It waits its turn instead of
    // cutting across the answer.
    const SpeechSubmission pushy =
        coordinator.Submit(Intent(SpeechOwner::Autonomy, SpeechBehavior::Interrupt));
    Check(pushy.state == SpeechIntentState::Queued,
        "An autonomous remark interrupted an untracked reply.");
    Check(channel.started.empty(), "Something was spoken over an active reply.");

    // System-critical speech still outranks a reply, probe or no probe. The one thing
    // that must always be audible is Revia saying she has stopped.
    const SpeechSubmission alarm =
        coordinator.Submit(Intent(SpeechOwner::System, SpeechBehavior::Interrupt));
    Check(alarm.state == SpeechIntentState::Speaking,
        "A stop announcement could not interrupt an untracked reply.");

    // Once the reply drains, what was waiting is heard -- after the answer, not over it.
    replyPlaying = false;
    channel.Finished(coordinator, alarm.id);
    Check(coordinator.Status().speaking &&
        coordinator.Status().activeOwner == SpeechOwner::Autonomy,
        "The remark that waited its turn never got one.");
    Check(channel.mostAudibleAtOnce == 1,
        "Untracked speech and coordinated speech overlapped.");
}

void TestTracingCarriesNoSpeechContent()
{
    // Diagnostics have to be safe to log. A trace that carries the sentence turns a
    // developer log into a transcript of private conversation.
    std::vector<SpeechTrace> steps;
    Channel channel;
    SpeechCoordinator coordinator;
    coordinator.SetChannel(channel.Wire());
    coordinator.SetTraceHandler([&](const SpeechTrace& step) { steps.push_back(step); });

    const std::string secret = "the passphrase is hunter2";
    coordinator.Submit(Intent(SpeechOwner::Conversation, SpeechBehavior::Queue, secret));
    Check(!steps.empty(), "Nothing was traced.");
    for (const SpeechTrace& step : steps)
    {
        Check(step.reason.find(secret) == std::string::npos,
            "A trace carried the text being spoken.");
        Check(step.characters == secret.size() || step.characters == 0,
            "A trace lost the length it is supposed to report.");
    }
}

} // namespace

void RunSpeechCoordinatorTests()
{
    TestUserSpeechInterruptsIdleChatter();
    TestChatterDoesNotInterruptConversation();
    TestSystemSpeechInterruptsNormalSpeech();
    TestQueuePreservesOrder();
    TestReplaceRemovesObsoleteSpeech();
    TestCancellationPreventsLaterPlayback();
    TestCanceledIntentCannotRestart();
    TestSingingOwnershipFollowsSettings();
    TestBargeInStopsCurrentSpeech();
    TestOwnersDoNotOverlap();
    TestSpeechTheCoordinatorDidNotStartStillHoldsTheFloor();
    TestTracingCarriesNoSpeechContent();
    std::cout << "Speech coordinator tests passed: one throat, and it is arbitrated.\n";
}
