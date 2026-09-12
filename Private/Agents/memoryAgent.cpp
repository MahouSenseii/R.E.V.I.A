#include "Agents/memoryAgent.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace revia::agents
{

namespace
{
    // The highest-volume Low-priority case by far: a scan can hand back dozens of
    // rows missing the same embedding model, and every one of them either succeeds
    // (nothing durable to protect; the row just keeps its place for the next scan) or
    // is a separate Critical failure event that this never touches.
    bool IsCoalescableBackfillSuccess(const MemoryAgentEvent& event)
    {
        return event.operation == "memory_backfill" && event.decision.bSuccess &&
            event.saveSucceeded;
    }
}

MemoryAgent::MemoryAgent() : worker([this](const std::stop_token stopToken)
{
    Run(stopToken);
}) {}

MemoryAgent::MemoryAgent(std::string memoryDatabasePath)
    : memory(std::move(memoryDatabasePath)),
      worker([this](const std::stop_token stopToken)
{
    Run(stopToken);
}) {}

MemoryAgent::~MemoryAgent()
{
    Stop();
}

std::string ToString(const MemoryTaskClass value)
{
    switch (value)
    {
        case MemoryTaskClass::InteractiveTurn: return "turn";
        case MemoryTaskClass::AutonomousLearning: return "learning";
        case MemoryTaskClass::EmbeddingBackfill: return "backfill";
    }
    return "turn";
}

MemoryTaskClass MemoryAgent::NextClass(
    const QueueDepths& depths,
    int& roundPosition,
    bool& outHasWork)
{
    outHasWork = depths.interactive > 0 || depths.learning > 0 || depths.backfill > 0;
    if (!outHasWork) return MemoryTaskClass::InteractiveTurn;

    // Four interactive slots, two learning, one backfill, repeating. Every class is
    // reached once per round whatever the others are doing, which is what makes
    // starvation impossible rather than merely unlikely.
    static constexpr std::array<MemoryTaskClass, 7> Round{
        MemoryTaskClass::InteractiveTurn,
        MemoryTaskClass::InteractiveTurn,
        MemoryTaskClass::AutonomousLearning,
        MemoryTaskClass::InteractiveTurn,
        MemoryTaskClass::InteractiveTurn,
        MemoryTaskClass::AutonomousLearning,
        MemoryTaskClass::EmbeddingBackfill};

    const auto available = [&depths](const MemoryTaskClass value)
    {
        switch (value)
        {
            case MemoryTaskClass::InteractiveTurn: return depths.interactive > 0;
            case MemoryTaskClass::AutonomousLearning: return depths.learning > 0;
            case MemoryTaskClass::EmbeddingBackfill: return depths.backfill > 0;
        }
        return false;
    };

    // The slot this round is on, if it has work.
    const int start = ((roundPosition % 7) + 7) % 7;
    if (available(Round[static_cast<std::size_t>(start)]))
    {
        roundPosition = start + 1;
        return Round[static_cast<std::size_t>(start)];
    }
    // Otherwise the next slot with work, scanning forward, so an empty class never
    // idles the worker while another has a backlog.
    for (int step = 1; step < 7; ++step)
    {
        const int position = (start + step) % 7;
        if (available(Round[static_cast<std::size_t>(position)]))
        {
            roundPosition = position + 1;
            return Round[static_cast<std::size_t>(position)];
        }
    }
    roundPosition = start + 1;
    return Round[static_cast<std::size_t>(start)];
}

void MemoryAgent::SetDiagnosticSink(DiagnosticSink sink)
{
    std::lock_guard lock(mutex);
    diagnostics = std::move(sink);
}

void MemoryAgent::SetQueueLimits(const MemoryQueueLimits newLimits)
{
    std::lock_guard lock(mutex);
    limits = newLimits;
}

MemoryAgent::QueueDepths MemoryAgent::Depths() const
{
    std::lock_guard lock(mutex);
    return {interactiveTasks.size(), learningTasks.size(), backfillTasks.size()};
}

MemoryEventPriority MemoryAgent::ClassifyEventPriority(const MemoryAgentEvent& event)
{
    if (event.operation == "memory_backfill")
    {
        // A failed embedding or a failed write to a memory that already exists is the
        // only way backfill loses ground; a success just means the next scan will not
        // find that row again, so this event alone protects nothing durable.
        return (!event.decision.bSuccess || !event.saveSucceeded)
            ? MemoryEventPriority::Critical
            : MemoryEventPriority::Low;
    }

    if (event.operation == "autonomous_learning")
    {
        // Semantic content was committed at acceptance. A later vector-write
        // failure must remain visible without claiming that content was lost.
        return event.saveSucceeded && event.embeddingError.empty()
            ? MemoryEventPriority::Important
            : MemoryEventPriority::Critical;
    }

    // "memory_evaluation": the interactive-turn classifier's verdict on one turn.
    if (!event.decision.bSuccess ||
        (event.decision.bShouldRemember && !event.saveSucceeded))
    {
        // The classifier itself failed to run, or it decided to remember and the save
        // failed. Either way, this event is the only record of a failure the session
        // would otherwise never see.
        return MemoryEventPriority::Critical;
    }
    if (event.decision.bShouldRemember && event.wasAdded)
    {
        return MemoryEventPriority::Important;
    }
    // Ran fine and either decided there was nothing worth keeping, or the memory
    // already existed under a prior summary.
    return MemoryEventPriority::Low;
}

std::size_t MemoryAgent::DroppedLowPriorityEvents() const
{
    std::lock_guard lock(mutex);
    return droppedLowPriorityEvents;
}

std::size_t MemoryAgent::CoalescedEvents() const
{
    std::lock_guard lock(mutex);
    return coalescedEvents;
}

std::size_t MemoryAgent::CriticalEventsEvicted() const
{
    std::lock_guard lock(mutex);
    return criticalEventsEvicted;
}

std::string MemoryAgent::AdmitEventLocked(MemoryAgentEvent event)
{
    if (events.size() < limits.maximumPendingEvents)
    {
        events.push_back(std::move(event));
        return {};
    }

    // At the bound. Prefer folding a new Low-priority backfill success into an
    // already-coalesced summary at the tail over evicting a different event outright:
    // a burst of hundreds of successful backfills is the highest-volume Low case, and
    // folding them costs nothing that mattered.
    if (IsCoalescableBackfillSuccess(event) && !events.empty() &&
        IsCoalescableBackfillSuccess(events.back()))
    {
        MemoryAgentEvent& tail = events.back();
        tail.coalescedCount = std::max<std::uint32_t>(tail.coalescedCount, 1) + 1;
        tail.turnId = event.turnId;
        ++coalescedEvents;
        return "[MemoryAgent] event_pressure | depth=" +
            std::to_string(limits.maximumPendingEvents) +
            " | action=coalesced | type=memory_backfill";
    }

    // Otherwise evict the oldest event at the lowest priority present, so a Critical
    // failure is never displaced while anything less important is still queued.
    if (const auto lowVictim = std::find_if(events.begin(), events.end(),
            [](const MemoryAgentEvent& queued)
            { return ClassifyEventPriority(queued) == MemoryEventPriority::Low; });
        lowVictim != events.end())
    {
        const std::string victimType = lowVictim->operation;
        events.erase(lowVictim);
        events.push_back(std::move(event));
        ++droppedLowPriorityEvents;
        return "[MemoryAgent] event_pressure | depth=" +
            std::to_string(limits.maximumPendingEvents) +
            " | action=coalesced | type=" + victimType;
    }

    if (const auto importantVictim = std::find_if(events.begin(), events.end(),
            [](const MemoryAgentEvent& queued)
            { return ClassifyEventPriority(queued) == MemoryEventPriority::Important; });
        importantVictim != events.end())
    {
        const std::string victimType = importantVictim->operation;
        events.erase(importantVictim);
        events.push_back(std::move(event));
        ++droppedLowPriorityEvents;
        return "[MemoryAgent] event_pressure | depth=" +
            std::to_string(limits.maximumPendingEvents) +
            " | action=evicted_important | type=" + victimType;
    }

    // Every queued event, and this one, are Critical. The bound still applies even
    // here: drop the oldest and say so loudly, rather than growing without limit. This
    // should not come up in practice -- it needs the entire event queue to be Critical
    // failures that nothing has drained -- but bounded takes precedence over it.
    const std::string victimType = events.front().operation;
    events.erase(events.begin());
    events.push_back(std::move(event));
    ++criticalEventsEvicted;
    return "[MemoryAgent] event_pressure | depth=" +
        std::to_string(limits.maximumPendingEvents) +
        " | action=evicted_critical | type=" + victimType;
}

void MemoryAgent::Report(const std::string& line) const
{
    DiagnosticSink sink;
    {
        std::lock_guard lock(mutex);
        sink = diagnostics;
    }
    // Called without the lock: the sink logs, and logging under the queue mutex would
    // put file I/O in front of every submission.
    if (sink) sink(line);
}

bool MemoryAgent::Enqueue(const MemoryTaskClass taskClass, Task task)
{
    std::string report;
    bool admitted = true;
    {
        std::lock_guard lock(mutex);
        if (worker.get_stop_token().stop_requested()) return false;
        switch (taskClass)
        {
            case MemoryTaskClass::InteractiveTurn:
            {
                if (interactiveTasks.size() >= limits.maximumInteractive)
                {
                    // The oldest pending evaluation is dropped rather than the newest,
                    // because the newest describes the conversation that is actually
                    // happening. This is a decision not yet made, not a memory already
                    // decided, and it is reported rather than absorbed.
                    interactiveTasks.pop_back();
                    report = "[MemoryAgent] overflow | type=turn | dropped=oldest | "
                        "depth=" + std::to_string(interactiveTasks.size() + 1);
                }
                interactiveTasks.push_front(std::move(task));
                break;
            }
            case MemoryTaskClass::AutonomousLearning:
            {
                if (learningTasks.size() >= limits.maximumLearning)
                {
                    // Content is already committed. Only optional vector work is
                    // deferred here; a later backfill can discover the saved row.
                    admitted = false;
                    report = "[MemoryAgent] overflow | type=learning | refused | depth=" +
                        std::to_string(learningTasks.size());
                    break;
                }
                if (!pendingEmbeddingIds.emplace(task.embeddingModel, task.memoryId).second)
                    return false;
                learningTasks.push_back(std::move(task));
                break;
            }
            case MemoryTaskClass::EmbeddingBackfill:
            {
                if (task.scheduledBackfill && (!backfill ||
                    task.backfillStop != backfill->cancellation.get_token() ||
                    task.backfillStop.stop_requested())) return false;
                if (backfillTasks.size() >= limits.maximumBackfill)
                {
                    admitted = false;
                    report = "[MemoryAgent] delayed | reason=queue_pressure | "
                        "type=backfill | depth=" + std::to_string(backfillTasks.size());
                    break;
                }
                if (!pendingEmbeddingIds.emplace(task.embeddingModel, task.memoryId).second)
                    return false;
                backfillTasks.push_back(std::move(task));
                break;
            }
        }
    }
    if (!report.empty()) Report(report);
    if (admitted) taskAvailable.notify_one();
    return admitted;
}

void MemoryAgent::Submit(
    const messageRouter& router,
    std::string input,
    std::string assistantResponse,
    const std::uint64_t turnId)
{
    if (input.empty() || worker.get_stop_token().stop_requested())
    {
        return;
    }

    Task task;
    task.router = &router;
    task.input = std::move(input);
    task.assistantResponse = std::move(assistantResponse);
    task.turnId = turnId;
    (void)Enqueue(MemoryTaskClass::InteractiveTurn, std::move(task));
    Report("[MemoryAgent] queued | type=turn | depth=" +
        std::to_string(Depths().interactive));
}

LearnedFindingResult MemoryAgent::SubmitLearnedFinding(
    const messageRouter& router,
    memoryDecision decision,
    const std::uint64_t turnId)
{
    if (!decision.bSuccess || !decision.bShouldRemember || decision.summary.empty() ||
        worker.get_stop_token().stop_requested())
    {
        return LearnedFindingResult::Failed;
    }

    // Acceptance is a database boundary, not a queue boundary. This is the same
    // curation/deduplication path used by the former overflow-only fallback.
    decision.embedding.clear();
    decision.embeddingModel.clear();
    bool wasAdded = false;
    std::string memoryId;
    const auto saveStarted = std::chrono::steady_clock::now();
    if (!memory.SaveAutomaticMemory(decision, wasAdded, &memoryId))
    {
        Report("[MemoryAgent] acceptance | type=learning | result=save_failed");
        return LearnedFindingResult::Failed;
    }
    decision.timings.push_back({"memory_db_save",
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - saveStarted).count()});

    if (!wasAdded)
    {
        const auto entries = memory.LoadMemories();
        const auto stored = std::find_if(entries.begin(), entries.end(),
            [&](const memoryEntry& entry) { return entry.id == memoryId; });
        if (stored == entries.end()) return LearnedFindingResult::AlreadyExists;
        // Deduplication retains the original text. Embed that text, not the
        // incoming restatement, including when the existing row has no vector.
        decision.summary = stored->summary;
    }

    Task task;
    task.router = &router;
    task.input = decision.summary;
    task.memoryId = std::move(memoryId);
    task.embeddingModel = router.EmbeddingModelName();
    task.learnedDecision = std::move(decision);
    task.hasLearnedDecision = true;
    task.learnedWasAdded = wasAdded;
    task.turnId = turnId;
    if (Enqueue(MemoryTaskClass::AutonomousLearning, std::move(task)))
    {
        Report("[MemoryAgent] queued | type=learning | content=saved | depth=" +
            std::to_string(Depths().learning));
        return LearnedFindingResult::SavedEmbeddingQueued;
    }

    if (!wasAdded)
    {
        Report("[MemoryAgent] overflow_fallback | type=learning | "
            "result=already_exists");
        return LearnedFindingResult::AlreadyExists;
    }
    Report("[MemoryAgent] overflow_fallback | type=learning | "
        "result=saved_without_embedding");
    return LearnedFindingResult::SavedWithoutEmbedding;
}

void MemoryAgent::SubmitEmbeddingBackfill(
    const messageRouter& router,
    const std::string& embeddingModel)
{
    if (embeddingModel.empty() || worker.get_stop_token().stop_requested())
    {
        return;
    }

    const std::vector<memoryEntry> missing =
        memory.LoadMissingEmbeddings(embeddingModel, 25);
    if (missing.empty())
    {
        return;
    }

    std::size_t admitted = 0;
    for (const memoryEntry& entry : missing)
    {
        Task task;
        task.router = &router;
        task.input = entry.summary;
        task.memoryId = entry.id;
        task.embeddingModel = embeddingModel;
        if (Enqueue(MemoryTaskClass::EmbeddingBackfill, std::move(task))) ++admitted;
    }
    Report("[MemoryAgent] queued | type=backfill | admitted=" +
        std::to_string(admitted) + " | scanned=" + std::to_string(missing.size()) +
        " | depth=" + std::to_string(Depths().backfill));
}

void MemoryAgent::StartEmbeddingBackfill(const messageRouter& router, const std::string& model)
{
    if (model.empty()) { StopEmbeddingBackfill(); return; }
    std::optional<std::stop_source> previous;
    {
        std::lock_guard lock(mutex);
        if (worker.get_stop_token().stop_requested()) return;
        if (backfill && backfill->router == &router && backfill->model == model) return;
        if (backfill) previous = backfill->cancellation;
        for (const auto& task : backfillTasks)
            pendingEmbeddingIds.erase({task.embeddingModel, task.memoryId});
        backfillTasks.clear();
        backfill.emplace();
        backfill->router = &router;
        backfill->model = model;
        backfill->nextScan = std::chrono::steady_clock::now();
    }
    if (previous) previous->request_stop();
    taskAvailable.notify_all();
}

void MemoryAgent::StopEmbeddingBackfill()
{
    std::optional<std::stop_source> previous;
    {
        std::lock_guard lock(mutex);
        if (backfill) previous = backfill->cancellation;
        backfill.reset();
        for (const auto& task : backfillTasks)
            pendingEmbeddingIds.erase({task.embeddingModel, task.memoryId});
        backfillTasks.clear();
    }
    if (previous) previous->request_stop();
    taskAvailable.notify_all();
}

bool MemoryAgent::BackfillDueLocked() const
{
    return backfill && backfill->pending == 0 &&
        std::chrono::steady_clock::now() >= backfill->nextScan;
}

std::chrono::milliseconds MemoryAgent::BackfillRetryDelay(int failures) const
{
    return std::min(backfillMaximumRetry,
        backfillRetryInterval * (1 << std::clamp(failures - 1, 0, 4)));
}

void MemoryAgent::ScanBackfill(std::stop_token stopToken)
{
    BackfillSubscription scan;
    std::size_t batchSize;
    {
        std::lock_guard lock(mutex);
        if (!BackfillDueLocked()) return;
        scan = *backfill;
        batchSize = std::min<std::size_t>(25, limits.maximumBackfill);
    }
    auto cancellation = scan.cancellation;
    std::stop_callback ownerStopped(stopToken, [&cancellation] { cancellation.request_stop(); });
    const auto token = cancellation.get_token();
    if (token.stop_requested()) return;
    const auto page = memory.ScanMissingEmbeddings(scan.model, scan.afterRowId, batchSize);
    std::string error = page.error;
    if (error.empty() && !page.entries.empty())
    {
        const auto health = scan.router->CheckEmbeddingHealth(token);
        if (!health.bIsAvailable) error = health.reason.empty() ? health.message : health.reason;
    }
    if (token.stop_requested()) return;
    if (!error.empty())
    {
        MemoryAgentEvent event;
        event.operation = "memory_backfill";
        event.decision.reason = error;
        std::string pressure;
        {
            std::lock_guard lock(mutex);
            if (!backfill || backfill->cancellation.get_token() != token) return;
            backfill->failures = std::min(backfill->failures + 1, 5);
            backfill->nextScan = std::chrono::steady_clock::now() + BackfillRetryDelay(backfill->failures);
            pressure = AdmitEventLocked(std::move(event));
        }
        Report("[MemoryAgent] backfill_scan | result=failed | retry=delayed");
        if (!pressure.empty()) Report(pressure);
        return;
    }
    std::size_t admitted = 0;
    for (const auto& entry : page.entries)
    {
        Task task;
        task.router = scan.router;
        task.input = entry.summary;
        task.memoryId = entry.id;
        task.embeddingModel = scan.model;
        task.backfillStop = token;
        task.scheduledBackfill = true;
        if (Enqueue(MemoryTaskClass::EmbeddingBackfill, std::move(task))) ++admitted;
    }
    {
        std::lock_guard lock(mutex);
        if (!backfill || backfill->cancellation.get_token() != token) return;
        backfill->afterRowId = page.nextRowId;
        backfill->atEnd = !page.hasMore;
        backfill->batchFailed = false;
        backfill->pending = admitted;
        if (admitted == 0)
        {
            if (backfill->atEnd) backfill->afterRowId = 0;
            backfill->nextScan = std::chrono::steady_clock::now() +
                (backfill->atEnd ? backfillIdleInterval : backfillBatchInterval);
        }
        else backfill->nextScan = std::chrono::steady_clock::time_point::max();
    }
    if (!page.entries.empty())
        Report("[MemoryAgent] backfill_scan | rows=" + std::to_string(page.entries.size()) +
            " | queued=" + std::to_string(admitted));
}

void MemoryAgent::FinishEmbeddingTask(const Task& task, bool failed)
{
    std::lock_guard lock(mutex);
    pendingEmbeddingIds.erase({task.embeddingModel, task.memoryId});
    if (!task.scheduledBackfill || !backfill ||
        backfill->cancellation.get_token() != task.backfillStop) return;
    backfill->batchFailed = backfill->batchFailed || failed;
    if (backfill->pending > 0) --backfill->pending;
    if (backfill->pending != 0) return;
    backfill->failures = backfill->batchFailed ? std::min(backfill->failures + 1, 5) : 0;
    auto delay = backfill->atEnd ? backfillIdleInterval : backfillBatchInterval;
    if (backfill->batchFailed) delay = std::max(delay, BackfillRetryDelay(backfill->failures));
    if (backfill->atEnd) backfill->afterRowId = 0;
    backfill->nextScan = std::chrono::steady_clock::now() + delay;
}

std::vector<MemoryAgentEvent> MemoryAgent::DrainEvents()
{
    std::lock_guard lock(mutex);
    std::vector<MemoryAgentEvent> drained;
    drained.swap(events);
    return drained;
}

void MemoryAgent::Stop()
{
    if (!worker.joinable())
    {
        return;
    }

    worker.request_stop();
    taskAvailable.notify_all();
    worker.join();
    std::lock_guard lock(mutex);
    interactiveTasks.clear();
    learningTasks.clear();
    backfillTasks.clear();
    pendingEmbeddingIds.clear();
    backfill.reset();
}

void MemoryAgent::Run(const std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        Task task;
        bool scanDue = false;
        {
            std::unique_lock lock(mutex);
            const auto ready = [&]()
            {
                return !interactiveTasks.empty() || !learningTasks.empty() ||
                    !backfillTasks.empty() || BackfillDueLocked();
            };
            if (backfill)
            {
                // A profile change can replace the subscription while the wait
                // releases the mutex; its deadline must remain a local value.
                const auto deadline = backfill->nextScan;
                taskAvailable.wait_until(lock, stopToken, deadline, ready);
            }
            else
                taskAvailable.wait(lock, stopToken, ready);
            if (stopToken.stop_requested())
            {
                interactiveTasks.clear();
                // Accepted findings are already in SQLite; only their optional
                // vector work is discarded here and can be backfilled later.
                learningTasks.clear();
                backfillTasks.clear();
                pendingEmbeddingIds.clear();
                return;
            }

            bool hasWork = false;
            const QueueDepths depths{
                interactiveTasks.size(), learningTasks.size(),
                backfillTasks.size() + (BackfillDueLocked() ? 1U : 0U)};
            const MemoryTaskClass chosen =
                NextClass(depths, roundPosition, hasWork);
            if (!hasWork) continue;
            switch (chosen)
            {
                case MemoryTaskClass::InteractiveTurn:
                    task = std::move(interactiveTasks.front());
                    interactiveTasks.pop_front();
                    break;
                case MemoryTaskClass::AutonomousLearning:
                    task = std::move(learningTasks.front());
                    learningTasks.pop_front();
                    break;
                case MemoryTaskClass::EmbeddingBackfill:
                    if (backfillTasks.empty())
                    {
                        scanDue = true;
                        break;
                    }
                    task = std::move(backfillTasks.front());
                    backfillTasks.pop_front();
                    break;
            }
        }
        if (scanDue)
        {
            ScanBackfill(stopToken);
            continue;
        }

        std::stop_source requestStop;
        std::stop_callback ownerStopped(stopToken, [&requestStop] { requestStop.request_stop(); });
        std::stop_callback backfillStopped(task.backfillStop, [&requestStop] { requestStop.request_stop(); });
        const auto requestToken = requestStop.get_token();

        if (!task.memoryId.empty() && !task.hasLearnedDecision)
        {
            if (requestToken.stop_requested() ||
                !memory.NeedsEmbedding(task.memoryId, task.embeddingModel))
            {
                FinishEmbeddingTask(task, false);
                continue;
            }
            const embeddingOutput embedding =
                task.router->EmbedMemory(task.input, requestToken);
            if (requestToken.stop_requested())
            {
                FinishEmbeddingTask(task, false);
                continue;
            }
            MemoryAgentEvent event;
            event.operation = "memory_backfill";
            event.decision.bSuccess = embedding.bSuccess && embedding.model == task.embeddingModel;
            event.decision.reason = embedding.bSuccess && !event.decision.bSuccess
                ? "The embedding model changed while backfill was pending." : embedding.reason;
            event.decision.timings.push_back({
                "memory_document_embedding",
                embedding.elapsedMilliseconds});
            if (event.decision.bSuccess &&
                memory.NeedsEmbedding(task.memoryId, task.embeddingModel) &&
                !requestToken.stop_requested())
            {
                const auto saveStarted = std::chrono::steady_clock::now();
                event.saveSucceeded = memory.SaveEmbedding(
                    task.memoryId,
                    embedding.model,
                    embedding.values);
                event.decision.timings.push_back({
                    "memory_db_save",
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - saveStarted).count()});
            }
            FinishEmbeddingTask(task, !event.decision.bSuccess || !event.saveSucceeded);
            std::string pressureReport;
            {
                std::lock_guard lock(mutex);
                pressureReport = AdmitEventLocked(std::move(event));
            }
            if (!pressureReport.empty()) Report(pressureReport);
            continue;
        }

        MemoryAgentEvent event;
        event.turnId = task.turnId;
        if (task.hasLearnedDecision)
        {
            event.operation = "autonomous_learning";
            event.decision = std::move(task.learnedDecision);
            event.wasAdded = task.learnedWasAdded;
            if (memory.NeedsEmbedding(task.memoryId, task.embeddingModel))
            {
                const embeddingOutput embedding = task.router->EmbedMemory(
                    event.decision.summary, requestToken);
                event.decision.timings.push_back({
                    "autonomous_learning_embedding", embedding.elapsedMilliseconds});
                if (embedding.bSuccess && !requestToken.stop_requested() &&
                    memory.NeedsEmbedding(task.memoryId, embedding.model))
                {
                    event.decision.embedding = embedding.values;
                    event.decision.embeddingModel = embedding.model;
                    const auto saveStarted = std::chrono::steady_clock::now();
                    if (!memory.SaveEmbedding(task.memoryId, embedding.model, embedding.values))
                    {
                        event.embeddingError = "The finding is saved, but its search vector could not be updated.";
                    }
                    event.decision.timings.push_back({"memory_embedding_save",
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - saveStarted).count()});
                }
            }
            FinishEmbeddingTask(task, !event.embeddingError.empty());
        }
        else
        {
            event.decision = task.router->EvaluateMemory(
                task.input, task.assistantResponse, stopToken);
        }
        if (stopToken.stop_requested())
        {
            return;
        }

        if (!task.hasLearnedDecision && event.decision.bSuccess && event.decision.bShouldRemember)
        {
            const auto saveStarted = std::chrono::steady_clock::now();
            event.saveSucceeded = memory.SaveAutomaticMemory(event.decision, event.wasAdded);
            event.decision.timings.push_back({
                "memory_db_save",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - saveStarted).count()});
        }

        std::string pressureReport;
        {
            std::lock_guard lock(mutex);
            pressureReport = AdmitEventLocked(std::move(event));
        }
        if (!pressureReport.empty()) Report(pressureReport);
    }
}

} // namespace revia::agents
