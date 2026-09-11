#include "Speech/speechCoordinator.h"

#include <algorithm>
#include <utility>

namespace revia::speech
{

namespace
{

constexpr std::size_t HistoryLimit = 256;

} // namespace

std::string ToString(const SpeechOwner owner)
{
    switch (owner)
    {
        case SpeechOwner::System: return "System";
        case SpeechOwner::Conversation: return "Conversation";
        case SpeechOwner::Research: return "Research";
        case SpeechOwner::Performance: return "Performance";
        case SpeechOwner::Skill: return "Skill";
        case SpeechOwner::Game: return "Game";
        case SpeechOwner::Autonomy: return "Autonomy";
    }
    return "Unknown";
}

std::string ToString(const SpeechBehavior behavior)
{
    switch (behavior)
    {
        case SpeechBehavior::Queue: return "Queue";
        case SpeechBehavior::Interrupt: return "Interrupt";
        case SpeechBehavior::Replace: return "Replace";
        case SpeechBehavior::IgnoreIfBusy: return "IgnoreIfBusy";
    }
    return "Unknown";
}

std::string ToString(const SpeechIntentState state)
{
    switch (state)
    {
        case SpeechIntentState::Created: return "Created";
        case SpeechIntentState::Queued: return "Queued";
        case SpeechIntentState::Speaking: return "Speaking";
        case SpeechIntentState::Completed: return "Completed";
        case SpeechIntentState::Interrupted: return "Interrupted";
        case SpeechIntentState::Replaced: return "Replaced";
        case SpeechIntentState::Canceled: return "Canceled";
        case SpeechIntentState::Ignored: return "Ignored";
    }
    return "Unknown";
}

int SpeechPriorities::For(const SpeechOwner owner) const
{
    switch (owner)
    {
        case SpeechOwner::System: return system;
        case SpeechOwner::Conversation: return conversation;
        case SpeechOwner::Performance: return performance;
        case SpeechOwner::Research: return research;
        case SpeechOwner::Skill: return skill;
        case SpeechOwner::Game: return game;
        case SpeechOwner::Autonomy: return autonomy;
    }
    return 0;
}

SpeechCoordinator::SpeechCoordinator()
    : startedAt(std::chrono::steady_clock::now())
{
}

void SpeechCoordinator::SetChannel(SpeechChannel value)
{
    const std::lock_guard<std::mutex> lock(mutex);
    channel = std::move(value);
}

void SpeechCoordinator::SetPriorities(SpeechPriorities value)
{
    const std::lock_guard<std::mutex> lock(mutex);
    priorities = value;
}

void SpeechCoordinator::SetSongPolicy(const SongPolicy value)
{
    const std::lock_guard<std::mutex> lock(mutex);
    songPolicy = value;
}

void SpeechCoordinator::SetTraceHandler(TraceHandler handler)
{
    const std::lock_guard<std::mutex> lock(mutex);
    trace = std::move(handler);
}

int SpeechCoordinator::PriorityOfLocked(const SpeechIntent& intent) const
{
    return intent.priority.has_value() ? *intent.priority : priorities.For(intent.owner);
}

void SpeechCoordinator::SetBusyProbe(BusyProbe probe)
{
    const std::lock_guard<std::mutex> lock(mutex);
    busyProbe = std::move(probe);
}

bool SpeechCoordinator::ChannelBusyLocked() const
{
    if (active.has_value() || performing) return true;
    // Sound this coordinator did not start still occupies the same throat.
    return busyProbe && busyProbe();
}

void SpeechCoordinator::RecordLocked(const TrackedIntent& tracked, const std::string& reason)
{
    history.push_back(tracked);
    if (history.size() > HistoryLimit)
    {
        history.erase(history.begin(), history.begin() + (history.size() - HistoryLimit));
    }
    if (!trace) return;

    SpeechTrace step;
    step.id = tracked.id;
    step.owner = tracked.owner;
    step.state = tracked.state;
    step.priority = tracked.priority;
    step.characters = tracked.characters;
    step.reason = reason;
    step.sinceStart = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);
    // Copied out and called under the lock. The handler is a diagnostics sink and must
    // not call back into the coordinator; that is cheaper to state than to defend
    // against with a second lock.
    trace(step);
}

SpeechSubmission SpeechCoordinator::Submit(SpeechIntent intent)
{
    const std::lock_guard<std::mutex> lock(mutex);

    Pending pending;
    pending.tracked.id = nextId++;
    pending.tracked.owner = intent.owner;
    pending.tracked.behavior = intent.behavior;
    pending.tracked.priority = PriorityOfLocked(intent);
    pending.tracked.state = SpeechIntentState::Created;
    pending.tracked.activityId = intent.activityId;
    pending.tracked.replaceKey = intent.replaceKey;
    pending.tracked.utteranceId = intent.utteranceId;
    pending.tracked.characters = intent.text.size();
    pending.tracked.createdAt = std::chrono::steady_clock::now();
    pending.intent = std::move(intent);

    SpeechSubmission submission;
    submission.id = pending.tracked.id;
    RecordLocked(pending.tracked, "submitted");

    if (pending.intent.text.empty() && pending.intent.owner != SpeechOwner::Performance)
    {
        pending.tracked.state = SpeechIntentState::Ignored;
        RecordLocked(pending.tracked, "there was nothing to say");
        submission.state = pending.tracked.state;
        submission.reason = "There was nothing to say.";
        return submission;
    }

    // A song holds the channel differently from an utterance: it is minutes long, and
    // whether it yields is a decision the user already made in settings rather than one
    // the priority ladder should quietly re-make.
    if (performing && pending.intent.owner != SpeechOwner::Performance)
    {
        if (!songPolicy.interruptSongToSpeak)
        {
            pending.tracked.state = SpeechIntentState::Ignored;
            RecordLocked(pending.tracked, "a song is playing and songs are set not to yield");
            submission.state = pending.tracked.state;
            submission.reason =
                "She is singing, and songs are set to keep playing, so this was not "
                "spoken.";
            return submission;
        }
        // Only something at least as important as answering the user stops the music.
        // Idle chatter interrupting a song is exactly the behaviour the setting exists
        // to prevent.
        if (pending.tracked.priority < priorities.conversation)
        {
            pending.tracked.state = SpeechIntentState::Ignored;
            RecordLocked(pending.tracked, "not important enough to stop a song");
            submission.state = pending.tracked.state;
            submission.reason = "She is singing, and this was not worth stopping for.";
            return submission;
        }
        if (channel.stopSong) channel.stopSong("she stopped singing to speak");
        performing = false;
        currentSongId.clear();
    }

    // Replace: this owner's older statements about the same thing are no longer true.
    if (pending.intent.behavior == SpeechBehavior::Replace)
    {
        const SpeechOwner owner = pending.intent.owner;
        const std::string key = pending.intent.replaceKey;
        RemoveQueuedLocked(
            [owner, key](const Pending& queued)
            {
                return queued.tracked.owner == owner &&
                    (key.empty() || queued.tracked.replaceKey == key);
            },
            SpeechIntentState::Replaced,
            "superseded by a newer statement from the same owner");
    }

    if (!ChannelBusyLocked())
    {
        BeginLocked(std::move(pending));
        submission.accepted = true;
        submission.state = SpeechIntentState::Speaking;
        submission.reason = "Speaking now.";
        return submission;
    }

    // Whatever holds the floor. Speech the coordinator did not start is treated as a
    // conversation reply, because that is what it is: the streaming reply path is the
    // only other thing that reaches SpeechService. Assuming it is unimportant would let
    // idle chatter cut across an answer to the user, which is the exact failure this
    // whole layer exists to prevent.
    const int holderPriority = active.has_value() ? active->tracked.priority
        : performing ? priorities.For(SpeechOwner::Performance)
        : priorities.conversation;

    switch (pending.intent.behavior)
    {
        case SpeechBehavior::IgnoreIfBusy:
        {
            pending.tracked.state = SpeechIntentState::Ignored;
            RecordLocked(pending.tracked, "the channel was busy and this would not wait");
            submission.state = pending.tracked.state;
            submission.reason = "She was already speaking, so this was dropped.";
            return submission;
        }
        case SpeechBehavior::Interrupt:
        case SpeechBehavior::Replace:
        {
            // Outranking is the whole test. An autonomous remark asking to interrupt a
            // reply to the user is refused here, which is what keeps "she talked over
            // you about nothing" from being reachable at all.
            if (pending.tracked.priority > holderPriority)
            {
                FinishActiveLocked(
                    SpeechIntentState::Interrupted,
                    "outranked by " + ToString(pending.tracked.owner));
                BeginLocked(std::move(pending));
                submission.accepted = true;
                submission.state = SpeechIntentState::Speaking;
                submission.reason = "Interrupted what was being said.";
                return submission;
            }
            break;
        }
        case SpeechBehavior::Queue:
            break;
    }

    // Ordered by priority, and stable within a priority so an owner's own sentences stay
    // in the order they were written. A critical line does not wait behind idle chatter;
    // two idle remarks stay in the order they were thought of.
    pending.tracked.state = SpeechIntentState::Queued;
    const auto position = std::find_if(
        queue.begin(), queue.end(),
        [&](const Pending& queued)
        {
            return queued.tracked.priority < pending.tracked.priority;
        });
    RecordLocked(pending.tracked, "waiting for the channel");
    submission.accepted = true;
    submission.state = SpeechIntentState::Queued;
    submission.reason = "Queued.";
    queue.insert(position, std::move(pending));
    return submission;
}

void SpeechCoordinator::BeginLocked(Pending pending)
{
    // Checked here rather than only at cancellation, because an intent can be withdrawn
    // while it sits in the queue and this is the single door it must pass through to
    // make a sound.
    if (canceled.contains(pending.tracked.id))
    {
        pending.tracked.state = SpeechIntentState::Canceled;
        RecordLocked(pending.tracked, "was canceled before it could start");
        return;
    }
    pending.tracked.state = SpeechIntentState::Speaking;
    RecordLocked(pending.tracked, "started");
    const SpeechIntent intent = pending.intent;
    const std::uint64_t id = pending.tracked.id;
    active = std::move(pending);
    if (channel.speak) channel.speak(intent, id);
}

void SpeechCoordinator::FinishActiveLocked(
    const SpeechIntentState state, const std::string& reason)
{
    if (!active.has_value()) return;
    TrackedIntent finished = active->tracked;
    finished.state = state;
    active.reset();
    if (state != SpeechIntentState::Completed && channel.stopSpeech)
    {
        channel.stopSpeech();
    }
    RecordLocked(finished, reason);
}

void SpeechCoordinator::StartNextLocked()
{
    while (!queue.empty() && !active.has_value() && !performing)
    {
        Pending next = std::move(queue.front());
        queue.pop_front();
        BeginLocked(std::move(next));
    }
}

void SpeechCoordinator::NotePlaybackFinished(const std::uint64_t id)
{
    const std::lock_guard<std::mutex> lock(mutex);
    // A late report about an utterance that was already interrupted must not end the one
    // that replaced it.
    if (!active.has_value() || active->tracked.id != id) return;
    FinishActiveLocked(SpeechIntentState::Completed, "finished");
    StartNextLocked();
}

void SpeechCoordinator::NotePerformanceStarted(const std::string& songId)
{
    const std::lock_guard<std::mutex> lock(mutex);
    performing = true;
    currentSongId = songId;
}

void SpeechCoordinator::NotePerformanceEnded()
{
    const std::lock_guard<std::mutex> lock(mutex);
    performing = false;
    currentSongId.clear();
    StartNextLocked();
}

void SpeechCoordinator::NoteUserSpoke()
{
    const std::lock_guard<std::mutex> lock(mutex);
    // Yield the floor, and take with it everything the user did not ask for. Leaving
    // autonomous remarks queued would mean talking over the user's sentence and then
    // finishing the thought afterwards, which is worse than either.
    if (active.has_value())
    {
        canceled.insert(active->tracked.id);
        FinishActiveLocked(SpeechIntentState::Interrupted, "the user started speaking");
    }
    const int floor = priorities.conversation;
    RemoveQueuedLocked(
        [floor](const Pending& queued) { return queued.tracked.priority < floor; },
        SpeechIntentState::Canceled,
        "dropped when the user started speaking");
}

void SpeechCoordinator::RemoveQueuedLocked(
    const std::function<bool(const Pending&)>& matches,
    const SpeechIntentState state,
    const std::string& reason)
{
    for (auto it = queue.begin(); it != queue.end();)
    {
        if (!matches(*it))
        {
            ++it;
            continue;
        }
        TrackedIntent removed = it->tracked;
        removed.state = state;
        canceled.insert(removed.id);
        RecordLocked(removed, reason);
        it = queue.erase(it);
    }
}

void SpeechCoordinator::Cancel(const std::uint64_t id, const std::string& reason)
{
    const std::lock_guard<std::mutex> lock(mutex);
    canceled.insert(id);
    if (active.has_value() && active->tracked.id == id)
    {
        FinishActiveLocked(SpeechIntentState::Canceled, reason);
        StartNextLocked();
        return;
    }
    RemoveQueuedLocked(
        [id](const Pending& queued) { return queued.tracked.id == id; },
        SpeechIntentState::Canceled, reason);
}

void SpeechCoordinator::CancelActivity(const std::string& activityId, const std::string& reason)
{
    if (activityId.empty()) return;
    const std::lock_guard<std::mutex> lock(mutex);
    if (active.has_value() && active->tracked.activityId == activityId)
    {
        canceled.insert(active->tracked.id);
        FinishActiveLocked(SpeechIntentState::Canceled, reason);
    }
    RemoveQueuedLocked(
        [&activityId](const Pending& queued)
        {
            return queued.tracked.activityId == activityId;
        },
        SpeechIntentState::Canceled, reason);
    StartNextLocked();
}

void SpeechCoordinator::CancelOwner(const SpeechOwner owner, const std::string& reason)
{
    const std::lock_guard<std::mutex> lock(mutex);
    if (active.has_value() && active->tracked.owner == owner)
    {
        canceled.insert(active->tracked.id);
        FinishActiveLocked(SpeechIntentState::Canceled, reason);
    }
    RemoveQueuedLocked(
        [owner](const Pending& queued) { return queued.tracked.owner == owner; },
        SpeechIntentState::Canceled, reason);
    StartNextLocked();
}

void SpeechCoordinator::CancelAll(const std::string& reason)
{
    const std::lock_guard<std::mutex> lock(mutex);
    if (active.has_value())
    {
        canceled.insert(active->tracked.id);
        FinishActiveLocked(SpeechIntentState::Canceled, reason);
    }
    RemoveQueuedLocked(
        [](const Pending&) { return true; }, SpeechIntentState::Canceled, reason);
}

SpeechChannelStatus SpeechCoordinator::Status() const
{
    const std::lock_guard<std::mutex> lock(mutex);
    SpeechChannelStatus status;
    status.speaking = active.has_value();
    status.performing = performing;
    status.queueDepth = queue.size();
    status.songId = currentSongId;
    if (active.has_value())
    {
        status.activeOwner = active->tracked.owner;
        status.activePriority = active->tracked.priority;
        status.activeId = active->tracked.id;
    }
    else if (performing)
    {
        status.activeOwner = SpeechOwner::Performance;
        status.activePriority = priorities.performance;
    }
    return status;
}

std::vector<TrackedIntent> SpeechCoordinator::History() const
{
    const std::lock_guard<std::mutex> lock(mutex);
    return history;
}

} // namespace revia::speech
