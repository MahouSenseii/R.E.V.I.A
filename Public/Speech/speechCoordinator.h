#pragma once

#include "Runtime/affectTypes.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace revia::speech
{

// One throat.
//
// Revia has several systems that may each decide, independently and at the same moment,
// that something should be said out loud: a conversation reply, a proposal from the
// initiative loop, a startup greeting, a song, and -- later -- games, streams, and
// skills. Until now each of them called SpeechService::Speak directly and the
// PerformanceRuntime played a song on a device of its own, which means the only thing
// preventing two of them from talking over each other was that they rarely happened to
// coincide.
//
// This is the layer that decides who owns the audio channel. It does not synthesise
// anything: SpeechService and its Qwen3-TTS pool remain the only speech backend, and
// PerformanceRuntime remains the only thing that plays a song. What this adds is the
// answer to "who is allowed to make sound right now, and what happens to everyone else".

enum class SpeechOwner
{
    // Emergency and status feedback. Outranks everything because the one thing that must
    // always be audible is Revia saying she has stopped.
    System,
    // The user is being answered. Nothing autonomous outranks this.
    Conversation,
    // A finished piece of work worth reporting.
    Research,
    // A song. Its own owner, because a performance is a timeline rather than an
    // utterance, and because whether it yields is a setting the user already owns.
    Performance,
    // An external integration with something to say.
    Skill,
    // A reaction to something happening in a game.
    Game,
    // Something she thought of herself. Deliberately near the bottom.
    Autonomy
};

// What a submitter wants to happen if the channel is already busy.
enum class SpeechBehavior
{
    // Wait for the floor.
    Queue,
    // Take the floor now, if this intent outranks what holds it.
    Interrupt,
    // Take the floor now and discard this owner's other pending intents. For an owner
    // whose older statements stop being true -- a status line, a score, a countdown.
    Replace,
    // Say it only if it can be said immediately. For remarks that are worth making now
    // or not at all.
    IgnoreIfBusy
};

enum class SpeechIntentState
{
    Created,
    Queued,
    Speaking,
    Completed,
    // Was speaking, and something outranked it.
    Interrupted,
    // Was queued, and its own owner superseded it.
    Replaced,
    // Withdrawn, by the submitter or by barge-in.
    Canceled,
    // Never queued: the channel was busy and the submitter said not to wait.
    Ignored
};

[[nodiscard]] std::string ToString(SpeechOwner owner);
[[nodiscard]] std::string ToString(SpeechBehavior behavior);
[[nodiscard]] std::string ToString(SpeechIntentState state);

// Where each owner sits. Defaults rather than literals at the decision site, so the
// ladder can be read in one place and adjusted from configuration without hunting for
// comparisons scattered through the arbitration.
struct SpeechPriorities
{
    int system = 100;
    int conversation = 80;
    int performance = 70;
    int research = 60;
    int skill = 50;
    int game = 45;
    int autonomy = 40;

    [[nodiscard]] int For(SpeechOwner owner) const;
};

// What a caller asks for. Text and audio both arrive here as "something to say"; the
// coordinator never looks inside it.
struct SpeechIntent
{
    SpeechOwner owner = SpeechOwner::Conversation;
    SpeechBehavior behavior = SpeechBehavior::Queue;
    std::string text;
    runtime::AffectSnapshot affect;
    // Correlates the resulting audio back to a reply, so a shell can reveal text in step
    // with sound. Passed through untouched.
    std::uint64_t utteranceId = 0;
    // The task or activity this belongs to, so cancelling a goal cancels what it was
    // about to say. Free-form because the callers that own tasks name them differently.
    std::string activityId;
    // For Replace: two intents from one owner sharing a key are the same statement, and
    // the newer one is the true one. Empty means "everything from this owner".
    std::string replaceKey;
    // Overrides the owner's place in the ladder. Present for the case the ladder cannot
    // express -- an unusually urgent skill message -- and absent almost always.
    std::optional<int> priority;
    bool latencyCritical = true;
};

// An intent the coordinator has accepted and given an identity.
struct TrackedIntent
{
    std::uint64_t id = 0;
    SpeechOwner owner = SpeechOwner::Conversation;
    SpeechBehavior behavior = SpeechBehavior::Queue;
    int priority = 0;
    SpeechIntentState state = SpeechIntentState::Created;
    std::string activityId;
    std::string replaceKey;
    std::uint64_t utteranceId = 0;
    std::size_t characters = 0;
    std::chrono::steady_clock::time_point createdAt{};
};

// What happened to a submission. Returned rather than thrown, because "she decided not
// to say that" is an ordinary outcome and not an error.
struct SpeechSubmission
{
    std::uint64_t id = 0;
    SpeechIntentState state = SpeechIntentState::Created;
    bool accepted = false;
    // Plain English, safe to show. Never contains the text.
    std::string reason;
};

// One step of an intent's life, for diagnostics.
//
// Deliberately carries no text. Speech content is private; how many characters there
// were and who wanted them said is what a developer needs to see a collision.
struct SpeechTrace
{
    std::uint64_t id = 0;
    SpeechOwner owner = SpeechOwner::Conversation;
    SpeechIntentState state = SpeechIntentState::Created;
    int priority = 0;
    std::size_t characters = 0;
    std::string reason;
    // Monotonic, since the coordinator was constructed. Wall clock would let a clock
    // adjustment reorder a trace.
    std::chrono::milliseconds sinceStart{0};
};

// What the coordinator is allowed to do to the world.
//
// Function objects rather than direct references to SpeechService and PerformanceRuntime
// so the arbitration can be tested without a voice, a GPU, or an audio device. The live
// wiring supplies the real ones.
struct SpeechChannel
{
    // Begin saying something. The coordinator calls this at most once per intent and
    // never while another intent is speaking.
    std::function<void(const SpeechIntent&, std::uint64_t id)> speak;
    // Stop whatever is being said. Must be safe to call when nothing is.
    std::function<void()> stopSpeech;
    // Stop the song, with a reason for the transcript.
    std::function<void(const std::string& reason)> stopSong;
};

// How singing behaves when something else wants to speak. Mirrors the existing
// performance setting rather than introducing a second one.
struct SongPolicy
{
    // False keeps the song running and leaves the reply on screen only, which is what
    // the existing interruptSongToSpeak setting already means.
    bool interruptSongToSpeak = true;
};

// A snapshot for developer diagnostics.
struct SpeechChannelStatus
{
    bool speaking = false;
    bool performing = false;
    SpeechOwner activeOwner = SpeechOwner::Conversation;
    int activePriority = 0;
    std::uint64_t activeId = 0;
    std::size_t queueDepth = 0;
    std::string songId;
};

class SpeechCoordinator
{
public:
    using TraceHandler = std::function<void(const SpeechTrace&)>;

    SpeechCoordinator();

    void SetChannel(SpeechChannel channel);
    void SetPriorities(SpeechPriorities priorities);
    void SetSongPolicy(SongPolicy policy);
    void SetTraceHandler(TraceHandler handler);

    // Asks the world whether the audio channel is busy, for sound the coordinator did
    // not start.
    //
    // The conversation reply path streams sentences into SpeechService as they are
    // generated, and that parallelism is what makes her answer quickly; routing it
    // through this queue would serialise synthesis and make every reply slower. So
    // rather than pretend that speech does not exist, the coordinator asks. An
    // autonomous remark is then correctly refused while a reply is still audible, even
    // though the reply never passed through here.
    //
    // The probe is called with the coordinator's lock held, so it must not call back
    // into the coordinator. SpeechService::HasPendingSpeech, the intended implementation,
    // does not.
    using BusyProbe = std::function<bool()>;
    void SetBusyProbe(BusyProbe probe);

    // The one way to make Revia say something.
    [[nodiscard]] SpeechSubmission Submit(SpeechIntent intent);

    // Called when the speech backend reports that an utterance finished playing. Anything
    // waiting starts here.
    void NotePlaybackFinished(std::uint64_t id);

    // The song took or released the channel. Called by whoever owns PerformanceRuntime.
    void NotePerformanceStarted(const std::string& songId);
    void NotePerformanceEnded();

    // The user started talking over her. Yields the floor and drops anything pending that
    // the user did not ask for.
    void NoteUserSpoke();

    // Withdraw one intent, everything belonging to a task, or everything at all. A
    // withdrawn intent never speaks, including one already waiting in the queue.
    void Cancel(std::uint64_t id, const std::string& reason);
    void CancelActivity(const std::string& activityId, const std::string& reason);
    void CancelOwner(SpeechOwner owner, const std::string& reason);
    void CancelAll(const std::string& reason);

    [[nodiscard]] SpeechChannelStatus Status() const;
    // Every intent this coordinator has seen, newest last. Bounded; for diagnostics only.
    [[nodiscard]] std::vector<TrackedIntent> History() const;

private:
    struct Pending
    {
        TrackedIntent tracked;
        SpeechIntent intent;
    };

    // Everything below runs with `mutex` already held.
    void StartNextLocked();
    void BeginLocked(Pending pending);
    void FinishActiveLocked(SpeechIntentState state, const std::string& reason);
    void RecordLocked(const TrackedIntent& tracked, const std::string& reason);
    void RemoveQueuedLocked(
        const std::function<bool(const Pending&)>& matches,
        SpeechIntentState state,
        const std::string& reason);
    [[nodiscard]] int PriorityOfLocked(const SpeechIntent& intent) const;
    [[nodiscard]] bool ChannelBusyLocked() const;

    mutable std::mutex mutex;
    SpeechChannel channel;
    SpeechPriorities priorities;
    SongPolicy songPolicy;
    TraceHandler trace;
    BusyProbe busyProbe;

    std::chrono::steady_clock::time_point startedAt;
    std::uint64_t nextId = 1;

    std::optional<Pending> active;
    std::deque<Pending> queue;

    // Ids that must never be spoken, even if a late caller still holds one. This is what
    // makes "a canceled intent cannot restart" true rather than merely likely.
    std::unordered_set<std::uint64_t> canceled;

    bool performing = false;
    std::string currentSongId;

    std::vector<TrackedIntent> history;
};

} // namespace revia::speech
