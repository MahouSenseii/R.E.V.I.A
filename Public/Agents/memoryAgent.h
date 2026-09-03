#pragma once

#include "Core/memoryManager.h"
#include "Core/messageRouter.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace revia::agents
{

    enum class LearnedFindingResult
    {
        Queued,
        SavedWithoutEmbedding,
        AlreadyExists,
        Failed
    };

// How much an event's disappearance under queue pressure actually costs.
//
// Critical and Important are named for what they describe, not for the queue: nothing
// here changes whether the underlying memory was saved, only whether the event
// reporting it is worth keeping when the bounded event queue is full.
enum class MemoryEventPriority
{
    // A failure that would otherwise vanish silently: a durable save that did not
    // happen, a classifier that did not run, an embedding that could not be computed.
    Critical,
    // Something durable actually happened: a new memory was saved, or an autonomous
    // finding was completed.
    Important,
    // Nothing changed, or a routine embedding backfill succeeded. The next scan finds
    // an unembedded row again regardless, so losing this event costs nothing durable.
    Low
};

struct MemoryAgentEvent
{
    memoryDecision decision;
    std::uint64_t turnId = 0;
    std::string operation = "memory_evaluation";
    bool saveSucceeded = true;
    bool wasAdded = false;
    // Set when this entry stands in for more than one Low-priority occurrence folded
    // together under queue pressure -- see MemoryAgent::AdmitEventLocked. Zero for an
    // ordinary event describing exactly one task.
    std::uint32_t coalescedCount = 0;
};

// What kind of work a queued memory task is.
//
// Explicit classes rather than a single queue with push_front for the urgent ones. The
// old shape put fresh turns at the head and everything else at the tail, so sustained
// conversation could keep jumping the queue indefinitely and the work behind it -- an
// autonomous finding, an embedding backfill -- was never reached at all.
enum class MemoryTaskClass
{
    // Classifying what just happened in conversation. Freshest work, highest priority:
    // a memory that arrives after the conversation moved on is worth much less.
    InteractiveTurn,
    // A finding Revia already decided to keep. The decision is made; only the storing
    // is pending, so this must never be discarded.
    AutonomousLearning,
    // Embedding a memory that already exists. Lowest value per task and the only class
    // that regenerates itself: a dropped backfill is found again by the next scan.
    EmbeddingBackfill
};

[[nodiscard]] std::string ToString(MemoryTaskClass value);

struct MemoryQueueLimits
{
    // Bounded per class rather than in total, so a flood of one kind cannot consume the
    // room another kind needs.
    std::size_t maximumInteractive = 64;
    // Generous, and never overflowed by dropping: these carry decisions already made.
    std::size_t maximumLearning = 256;
    std::size_t maximumBackfill = 256;
    // Drained by the session each poll. Capped so a shell that stops draining cannot
    // grow this without bound either.
    std::size_t maximumPendingEvents = 512;
};

class MemoryAgent
{
public:
    MemoryAgent();
    // Same as the default constructor, but the durable store lives at
    // `memoryDatabasePath` instead of the process-relative default. Exists for tests
    // that need an isolated database rather than the shared runtime one.
    explicit MemoryAgent(std::string memoryDatabasePath);
    ~MemoryAgent();

    MemoryAgent(const MemoryAgent&) = delete;
    MemoryAgent& operator=(const MemoryAgent&) = delete;

    void Submit(
        const messageRouter& router,
        std::string input,
        std::string assistantResponse = "",
        std::uint64_t turnId = 0);
    // Stores one already-grounded, bounded finding without asking the conversation
    // classifier to reinterpret web text. Embedding still runs on the background lane.
    //
    // Never destroys an already-approved decision: when the learning queue is full,
    // this saves it immediately without an embedding rather than refusing it. The
    // existing backfill scan adds the vector later. The returned disposition tells the
    // caller which of those happened, so autonomy reporting never claims a save that
    // did not happen.
    [[nodiscard]] LearnedFindingResult SubmitLearnedFinding(
        const messageRouter& router,
        memoryDecision decision,
        std::uint64_t turnId = 0);
    void SubmitEmbeddingBackfill(
        const messageRouter& router,
        const std::string& embeddingModel);
    std::vector<MemoryAgentEvent> DrainEvents();
    void Stop();

    // Plain diagnostic lines: queue depth, delay, overflow. Never task content.
    using DiagnosticSink = std::function<void(const std::string&)>;
    void SetDiagnosticSink(DiagnosticSink sink);
    void SetQueueLimits(MemoryQueueLimits limits);

    // Queue depth per class, for tests and for the resources panel.
    struct QueueDepths
    {
        std::size_t interactive = 0;
        std::size_t learning = 0;
        std::size_t backfill = 0;
    };
    [[nodiscard]] QueueDepths Depths() const;

    // The scheduling order, exposed so fairness can be tested without a router, a
    // model, or a database. Given how many tasks of each class are waiting, returns the
    // class the worker takes next and advances the round.
    //
    // A weighted round robin over a fixed round of seven slots -- four interactive, two
    // learning, one backfill. Every class is reached in every round, so none can starve
    // however busy the others are, while fresh conversation still gets most of the
    // worker. An empty slot falls through to whichever class has work, so the weights
    // never idle the worker.
    [[nodiscard]] static MemoryTaskClass NextClass(
        const QueueDepths& depths,
        int& roundPosition,
        bool& outHasWork);

    // What the event queue's bounded eviction would do with this event, exposed so the
    // policy can be tested directly: Critical events are never coalesced away, Low ones
    // are the first to go, and Important sits between the two.
    [[nodiscard]] static MemoryEventPriority ClassifyEventPriority(
        const MemoryAgentEvent& event);

    // How many Low-priority (or, failing that, Important) events have been coalesced
    // or evicted to keep the event queue bounded under pressure, and how many times
    // pressure was severe enough that even a Critical event had to be dropped -- the
    // one condition that should never come up in practice, since Critical events are
    // preserved as long as anything lower-priority is still queued. Never task content.
    [[nodiscard]] std::size_t DroppedLowPriorityEvents() const;
    [[nodiscard]] std::size_t CoalescedEvents() const;
    [[nodiscard]] std::size_t CriticalEventsEvicted() const;

private:
    struct Task
    {
        const messageRouter* router = nullptr;
        std::string input;
        std::string assistantResponse;
        std::string memoryId;
        memoryDecision learnedDecision;
        bool hasLearnedDecision = false;
        std::uint64_t turnId = 0;
    };

    void Run(std::stop_token stopToken);
    void Report(const std::string& line) const;
    // Returns false when the queue is full and the task could not be admitted.
    bool Enqueue(MemoryTaskClass taskClass, Task task);
    // Appends one event, coalescing or evicting under the priority policy when the
    // queue is already at its bound. Requires `mutex`; returns a diagnostic line to
    // report once the caller has released it, or an empty string when nothing
    // noteworthy happened.
    std::string AdmitEventLocked(MemoryAgentEvent event);

    memoryManager memory;
    mutable std::mutex mutex;
    std::condition_variable_any taskAvailable;
    std::deque<Task> interactiveTasks;
    std::deque<Task> learningTasks;
    std::deque<Task> backfillTasks;
    // Memory ids already queued for embedding, so a repeated scan cannot enqueue the
    // same row again. Erased when the task is taken.
    std::unordered_set<std::string> queuedBackfillIds;
    int roundPosition = 0;
    std::vector<MemoryAgentEvent> events;
    MemoryQueueLimits limits;
    DiagnosticSink diagnostics;
    // Guarded by `mutex`, same as `events`.
    std::size_t droppedLowPriorityEvents = 0;
    std::size_t coalescedEvents = 0;
    std::size_t criticalEventsEvicted = 0;
    std::jthread worker;
};

} // namespace revia::agents
