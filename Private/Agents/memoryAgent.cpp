#include "LLM/embeddingTypes.h"
#include "Memory/memoryTypes.h"
#include "Agents/memoryAgent.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
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
        return (event.operation == "memory_backfill" || event.operation == "archive_backfill") && event.decision.bSuccess &&
            event.saveSucceeded;
    }
}

MemoryAgent::MemoryAgent() : worker([this](const std::stop_token stopToken) { Run(stopToken); })
{
}

MemoryAgent::MemoryAgent(std::string memoryDatabasePath)
    : memory(std::filesystem::absolute(memoryDatabasePath).lexically_normal().string()),
      worker([this](const std::stop_token stopToken) { Run(stopToken); })
{
}

MemoryAgent::~MemoryAgent()
{
    Stop();
}

void MemoryAgent::ConfigureSemanticDuplicates(memory::SemanticDuplicateSettings settings,
    std::shared_ptr<memory::SemanticDuplicateScorer> scorer, memory::MemoryScope scope,
    std::function<bool()> contextAdmission)
{
    std::optional<DuplicateConfiguration> retired;
    {
        std::lock_guard lock(mutex);
        retired = std::move(duplicates);
        duplicates.reset();
        if (scorer && memory::ValidSemanticDuplicateSettings(settings) && memory::IsAttributedPrivateScope(scope))
            duplicates = DuplicateConfiguration{std::move(settings), std::move(scorer), std::move(scope),
                std::move(contextAdmission), {}};
    }
    if (retired) retired->cancellation.request_stop();
}

void MemoryAgent::DisableSemanticDuplicates()
{
    ConfigureSemanticDuplicates({}, {}, {});
}

std::size_t MemoryAgent::MarkSemanticDuplicates(const std::string& id, const std::string& model,
    const Task& task, std::stop_token ownerToken)
{
    std::optional<DuplicateConfiguration> configuration;
    {
        std::lock_guard lock(mutex);
        configuration = duplicates;
    }
    if (!configuration || task.archive || id.empty() || model.empty()) return 0;
    // Only curated scans have no captured conversational scope. An explicitly
    // non-private task cannot borrow a formerly private configuration.
    const bool hasCapturedScope = !task.scope.participantId.empty() || !task.scope.companionId.empty() ||
        !task.scope.audience.audienceId.empty() || task.scope.audience.kind != identity::AudienceKind::Unknown;
    if (hasCapturedScope && !memory::IsAttributedPrivateScope(task.scope)) return 0;
    // A captured private task may not borrow a different participant's later scope.
    if (memory::IsAttributedPrivateScope(task.scope) &&
        (task.scope.participantId != configuration->scope.participantId ||
         task.scope.companionId != configuration->scope.companionId ||
         task.scope.audience.audienceId != configuration->scope.audience.audienceId ||
         task.scope.audience.revision != configuration->scope.audience.revision ||
         task.scope.audience.recipientEntityIds != configuration->scope.audience.recipientEntityIds ||
         task.scope.participantSource != configuration->scope.participantSource ||
         task.scope.consentRevision != configuration->scope.consentRevision)) return 0;
    std::stop_source cancellation;
    std::stop_callback ownerStopped(ownerToken, [&] { cancellation.request_stop(); });
    std::stop_callback configurationStopped(configuration->cancellation.get_token(), [&] { cancellation.request_stop(); });
    const auto admission = [&] { return !cancellation.stop_requested() && Admitted(task.admission) && Admitted(configuration->admission); };
    try
    {
        return memory.CheckSemanticDuplicates(id, model, configuration->scope, configuration->settings,
            configuration->scorer.get(), cancellation.get_token(), admission).marked;
    }
    catch (...) { return 0; } // Optional advisory work never changes save disposition.
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

MemoryTaskClass MemoryAgent::NextClass(const QueueDepths& depths, int& roundPosition, bool& outHasWork)
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

bool MemoryAgent::Admitted(const std::function<bool()>& guard)
{
    try
    {
        return !guard || guard();
    }
    catch (...)
    {
        return false;
    }
}

std::function<bool()> MemoryAgent::CaptureAdmission() const
{
    std::lock_guard lock(mutex);
    return admissionGuard;
}

void MemoryAgent::SetAdmissionGuard(std::function<bool()> guard)
{
    std::lock_guard lock(mutex);
    admissionGuard = std::move(guard);
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

void MemoryAgent::SetDeferral(MemoryDeferral newDeferral)
{
    {
        std::lock_guard lock(mutex);
        deferral = std::move(newDeferral);
    }
    taskAvailable.notify_all();
}

MemoryAgent::QueueDepths MemoryAgent::Depths() const
{
    std::lock_guard lock(mutex);
    return {interactiveTasks.size(), learningTasks.size(), backfillTasks.size()};
}

MemoryEventPriority MemoryAgent::ClassifyEventPriority(const MemoryAgentEvent& event)
{
    if (event.operation == "memory_backfill" || event.operation == "archive_backfill")
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
    if ((event.decision.bShouldRemember && event.wasAdded) || !event.correctionProposals.empty())
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

std::string MemoryAgent::AdmitEventLocked(MemoryAgentEvent value, std::function<bool()> guard)
{
    // Retired guards are filtered when events leave the queue. Invoking a scope
    // callback here would invert the session's scope -> queue lock order.
    PendingEvent event;
    static_cast<MemoryAgentEvent&>(event) = std::move(value);
    event.admission = std::move(guard);
    if (events.size() < limits.maximumPendingEvents)
    {
        events.push_back(std::move(event));
        return {};
    }

    // At the bound. Prefer folding a new Low-priority backfill success into an
    // already-coalesced summary at the tail over evicting a different event outright:
    // a burst of hundreds of successful backfills is the highest-volume Low case, and
    // folding them costs nothing that mattered.
    if (!event.admission && IsCoalescableBackfillSuccess(event) && !events.empty() && !events.back().admission &&
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
    if (sink && Admitted(CaptureAdmission())) sink(line);
}

bool MemoryAgent::Enqueue(const MemoryTaskClass taskClass, Task task)
{
    // Admission can acquire session scope locks. Never invoke it under the queue
    // mutex; the worker and durable writes revalidate after dequeue as well.
    if (!Admitted(task.admission)) return false;
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
                task.queuedAt = std::chrono::steady_clock::now();
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
                if (!pendingEmbeddingIds.insert(EmbeddingKey(task)).second)
                    return false;
                learningTasks.push_back(std::move(task));
                break;
            }
            case MemoryTaskClass::EmbeddingBackfill:
            {
                const auto& subscription = task.archive ? archiveBackfill : backfill;
                if (task.scheduledBackfill && (!subscription ||
                    task.backfillStop != subscription->cancellation.get_token() ||
                    task.backfillStop.stop_requested())) return false;
                if (backfillTasks.size() >= limits.maximumBackfill)
                {
                    admitted = false;
                    report = "[MemoryAgent] delayed | reason=queue_pressure | "
                        "type=backfill | depth=" + std::to_string(backfillTasks.size());
                    break;
                }
                if (!pendingEmbeddingIds.insert(EmbeddingKey(task)).second)
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

void MemoryAgent::Submit(const messageRouter& router, std::string input, std::string assistantResponse, const ResponseProvenance provenance,
    const std::uint64_t turnId)
{
    Submit(router, std::move(input), std::move(assistantResponse), provenance, turnId, {});
}

void MemoryAgent::Submit(const messageRouter& router, std::string input, std::string assistantResponse, const ResponseProvenance provenance,
    const std::uint64_t turnId, std::function<bool()> contextAdmission, const memory::MemoryScope& scope)
{
    if (input.empty() || worker.get_stop_token().stop_requested())
    {
        return;
    }

    Task task;
    task.admission = [sessionAdmission = CaptureAdmission(), contextAdmission = std::move(contextAdmission)]
    {
        return Admitted(sessionAdmission) && Admitted(contextAdmission);
    };
    if (!Admitted(task.admission)) return;
    task.router = &router;
    task.input = std::move(input);
    task.assistantResponse = std::move(assistantResponse);
    task.provenance = provenance;
    task.scope = scope;
    task.turnId = turnId;
    (void)Enqueue(MemoryTaskClass::InteractiveTurn, std::move(task));
    Report("[MemoryAgent] queued | type=turn | depth=" +
        std::to_string(Depths().interactive));
}

LearnedFindingResult MemoryAgent::SubmitLearnedFinding(const messageRouter& router, memoryDecision decision, const std::uint64_t turnId)
{
    return SubmitLearnedFinding(router, std::move(decision), turnId, nullptr);
}

LearnedFindingResult MemoryAgent::SubmitLearnedFinding(const messageRouter& router, memoryDecision decision, const std::uint64_t turnId,
    std::string* outMemoryId, std::function<bool()> contextAdmission)
{
    if (outMemoryId)
        outMemoryId->clear();
    if (!decision.bSuccess || !decision.bShouldRemember || decision.summary.empty() ||
        worker.get_stop_token().stop_requested())
    {
        return LearnedFindingResult::Failed;
    }

    const auto sessionAdmission = CaptureAdmission();
    const auto guard = [sessionAdmission, contextAdmission = std::move(contextAdmission)]
    { return Admitted(sessionAdmission) && Admitted(contextAdmission); };
    if (!Admitted(guard)) return LearnedFindingResult::Failed;

    // Acceptance is a database boundary, not a queue boundary. This is the same
    // curation/deduplication path used by the former overflow-only fallback.
    decision.embedding.clear();
    decision.embeddingModel.clear();
    bool wasAdded = false;
    std::string memoryId;
    const auto saveStarted = std::chrono::steady_clock::now();
    if (!Admitted(guard) || !memory.SaveAutomaticMemory(decision, wasAdded, &memoryId, guard))
    {
        Report("[MemoryAgent] acceptance | type=learning | result=save_failed");
        return LearnedFindingResult::Failed;
    }
    decision.timings.push_back({"memory_db_save",
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - saveStarted).count()});

    if (outMemoryId)
        *outMemoryId = memoryId;

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
    task.admission = guard;
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

LearnedFindingResult MemoryAgent::SubmitOwnerMemoryRevision(const messageRouter& router, const memory::MemoryRevisionRequest& request,
    memory::MemoryRevisionReceipt& receipt, std::string& error, const std::stop_token stopToken,
    memory::MemoryRevisionScopeAdmission acquireReviewScope)
{
    receipt = {};
    const auto guard = [this, origin = CaptureAdmission(), stopToken]
    { return !worker.get_stop_token().stop_requested() && !stopToken.stop_requested() && Admitted(origin); };
    if (worker.get_stop_token().stop_requested() || !Admitted(guard))
    {
        error = "Owner memory revision belongs to an inactive memory worker.";
        return LearnedFindingResult::Failed;
    }
    if (!memory.SaveOwnerRevision(request, receipt, error, guard, std::move(acquireReviewScope)))
        return LearnedFindingResult::Failed;
    // The semantic revision and its receipt already committed. Backfill on the
    // existing worker recovers any optional vector work after queue pressure or Stop.
    if (!stopToken.stop_requested() && !router.EmbeddingModelName().empty())
        SubmitEmbeddingBackfill(router, router.EmbeddingModelName());
    return receipt.wasAdded ? LearnedFindingResult::SavedWithoutEmbedding : LearnedFindingResult::AlreadyExists;
}

void MemoryAgent::SubmitEmbeddingBackfill(const messageRouter& router, const std::string& embeddingModel)
{
    if (embeddingModel.empty() || worker.get_stop_token().stop_requested())
    {
        return;
    }

    const auto guard = CaptureAdmission();
    if (!Admitted(guard)) return;
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
        task.admission = guard;
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
    const auto admitted = CaptureAdmission();
    if (!Admitted(admitted)) return;
    std::optional<std::stop_source> previous;
    {
        std::lock_guard lock(mutex);
        if (worker.get_stop_token().stop_requested()) return;
        if (backfill && backfill->router == &router && backfill->model == model) return;
        if (backfill) previous = backfill->cancellation;
        std::erase_if(backfillTasks, [&](const Task& task)
        {
            if (task.archive) return false;
            pendingEmbeddingIds.erase(EmbeddingKey(task));
            return true;
        });
        // Assigned rather than emplaced: clang cannot default-construct a nested type
        // with member initializers until the enclosing class is complete.
        backfill = BackfillSubscription{};
        backfill->admission = admitted;
        backfill->router = &router;
        backfill->model = model;
        backfill->nextScan = std::chrono::steady_clock::now();
    }
    if (previous) previous->request_stop();
    taskAvailable.notify_all();
}

void MemoryAgent::StartArchiveEmbeddingBackfill(const messageRouter& router, memory::ConversationArchive& archive,
    const std::string& model, const memory::MemoryScope& scope, std::function<bool()> contextAdmission)
{
    if (model.empty() || !memory::IsAttributedPrivateScope(scope))
    {
        StopArchiveEmbeddingBackfill();
        return;
    }
    std::optional<BackfillSubscription> observed;
    {
        std::lock_guard lock(mutex);
        observed = archiveBackfill;
    }
    const bool observedAdmitted = observed && Admitted(observed->admission);
    const auto guard = [sessionAdmission = CaptureAdmission(), contextAdmission = std::move(contextAdmission)]
    {
        return Admitted(sessionAdmission) && Admitted(contextAdmission);
    };
    const bool mayStart = Admitted(guard);
    std::optional<std::stop_source> previous;
    {
        std::lock_guard lock(mutex);
        if (worker.get_stop_token().stop_requested()) return;
        if (mayStart && observedAdmitted && archiveBackfill && observed &&
            archiveBackfill->cancellation.get_token() == observed->cancellation.get_token() &&
            archiveBackfill->router == &router && archiveBackfill->archive == &archive &&
            archiveBackfill->model == model && archiveBackfill->scope.participantId == scope.participantId &&
            archiveBackfill->scope.audience.kind == scope.audience.kind &&
            archiveBackfill->scope.audience.audienceId == scope.audience.audienceId &&
            archiveBackfill->scope.audience.revision == scope.audience.revision &&
            archiveBackfill->scope.participantSource == scope.participantSource &&
            archiveBackfill->scope.consentRevision == scope.consentRevision &&
            archiveBackfill->scope.companionId == scope.companionId)
            return;
        if (archiveBackfill) previous = archiveBackfill->cancellation;
        std::erase_if(backfillTasks, [&](const Task& task)
        {
            if (!task.archive) return false;
            pendingEmbeddingIds.erase(EmbeddingKey(task));
            return true;
        });
        archiveBackfill = BackfillSubscription{};
        archiveBackfill->admission = guard;
        if (!mayStart)
            archiveBackfill.reset();
        else
        {
            archiveBackfill->router = &router;
            archiveBackfill->archive = &archive;
            archiveBackfill->scope = scope;
            archiveBackfill->model = model;
            archiveBackfill->nextScan = std::chrono::steady_clock::now();
        }
    }
    if (previous) previous->request_stop();
    taskAvailable.notify_all();
}

void MemoryAgent::StopArchiveEmbeddingBackfill()
{
    std::optional<std::stop_source> previous;
    {
        std::lock_guard lock(mutex);
        if (archiveBackfill) previous = archiveBackfill->cancellation;
        archiveBackfill.reset();
        std::erase_if(backfillTasks, [&](const Task& task)
        {
            if (!task.archive) return false;
            pendingEmbeddingIds.erase(EmbeddingKey(task));
            return true;
        });
    }
    if (previous) previous->request_stop();
    taskAvailable.notify_all();
}

void MemoryAgent::StopEmbeddingBackfill()
{
    std::optional<std::stop_source> previous;
    std::optional<std::stop_source> previousArchive;
    {
        std::lock_guard lock(mutex);
        if (backfill) previous = backfill->cancellation;
        if (archiveBackfill) previousArchive = archiveBackfill->cancellation;
        backfill.reset();
        archiveBackfill.reset();
        for (const auto& task : backfillTasks)
            pendingEmbeddingIds.erase(EmbeddingKey(task));
        backfillTasks.clear();
    }
    if (previous) previous->request_stop();
    if (previousArchive) previousArchive->request_stop();
    taskAvailable.notify_all();
}

bool MemoryAgent::InteractiveDueLocked(
    const std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point& wakeAt) const
{
    if (interactiveTasks.empty())
        return false;
    if (!deferral.busy)
        return true;
    // The front is the task taken next: the newest turn, or a retry when nothing newer waits.
    const auto queuedAt = interactiveTasks.front().queuedAt;
    const auto latest = queuedAt + deferral.maximum;
    if (now >= latest)
        return true;
    if (now < queuedAt + deferral.settle)
    {
        wakeAt = queuedAt + deferral.settle;
        return false;
    }
    bool busy = true;
    try
    {
        busy = deferral.busy();
    }
    catch (...)
    {
        // An unreadable probe only delays the work; the maximum still bounds it.
    }
    if (!busy)
        return true;
    wakeAt = std::min(latest, now + deferral.recheck);
    return false;
}

bool MemoryAgent::BackfillDueLocked() const
{
    const auto now = std::chrono::steady_clock::now();
    return (backfill && backfill->pending == 0 && now >= backfill->nextScan) ||
        (archiveBackfill && archiveBackfill->pending == 0 && now >= archiveBackfill->nextScan);
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
        const auto now = std::chrono::steady_clock::now();
        const bool curatedDue = backfill && backfill->pending == 0 && now >= backfill->nextScan;
        const bool archiveDue = archiveBackfill && archiveBackfill->pending == 0 && now >= archiveBackfill->nextScan;
        const bool useArchive = archiveDue && (!curatedDue || preferArchiveScan);
        scan = useArchive ? *archiveBackfill : *backfill;
        preferArchiveScan = !useArchive;
        batchSize = std::min<std::size_t>(25, limits.maximumBackfill);
    }
    auto cancellation = scan.cancellation;
    std::stop_callback ownerStopped(stopToken, [&cancellation] { cancellation.request_stop(); });
    const auto token = cancellation.get_token();
    if (token.stop_requested() || !Admitted(scan.admission)) return;
    std::vector<Task> missing;
    std::string error;
    std::int64_t nextRowId = scan.afterRowId;
    bool hasMore = false;
    const auto makeTask = [&]()
    {
        Task task;
        task.admission = scan.admission;
        task.router = scan.router;
        task.archive = scan.archive;
        task.scope = scan.scope;
        task.embeddingModel = scan.model;
        task.backfillStop = token;
        task.scheduledBackfill = true;
        return task;
    };
    if (scan.archive)
    {
        const auto page = scan.archive->ScanMissingEmbeddings(scan.model, scan.afterRowId, batchSize, &scan.scope);
        error = page.error;
        nextRowId = page.nextRowId;
        hasMore = page.hasMore;
        for (const auto& entry : page.entries)
        {
            auto task = makeTask();
            task.input = memory::ArchiveEmbeddingText(entry);
            task.memoryId = entry.id;
            task.archivedTurn = entry;
            missing.push_back(std::move(task));
        }
    }
    else
    {
        const auto page = memory.ScanMissingEmbeddings(scan.model, scan.afterRowId, batchSize);
        error = page.error;
        nextRowId = page.nextRowId;
        hasMore = page.hasMore;
        for (const auto& entry : page.entries)
        {
            auto task = makeTask();
            task.input = entry.summary;
            task.memoryId = entry.id;
            missing.push_back(std::move(task));
        }
    }
    if (error.empty() && !missing.empty())
    {
        const auto health = scan.router->CheckEmbeddingHealth(token);
        if (!health.bIsAvailable) error = health.reason.empty() ? health.message : health.reason;
    }
    if (token.stop_requested() || !Admitted(scan.admission)) return;
    if (!error.empty())
    {
        MemoryAgentEvent event;
        event.operation = scan.archive ? "archive_backfill" : "memory_backfill";
        event.decision.reason = error;
        std::string pressure;
        {
            std::lock_guard lock(mutex);
            auto& subscription = scan.archive ? archiveBackfill : backfill;
            if (!subscription || subscription->cancellation.get_token() != token) return;
            subscription->failures = std::min(subscription->failures + 1, 5);
            subscription->nextScan = std::chrono::steady_clock::now() + BackfillRetryDelay(subscription->failures);
            pressure = AdmitEventLocked(std::move(event), scan.admission);
        }
        Report("[MemoryAgent] backfill_scan | result=failed | retry=delayed");
        if (!pressure.empty()) Report(pressure);
        return;
    }
    std::size_t admitted = 0;
    for (auto& task : missing)
    {
        if (Enqueue(MemoryTaskClass::EmbeddingBackfill, std::move(task))) ++admitted;
    }
    {
        std::lock_guard lock(mutex);
        auto& subscription = scan.archive ? archiveBackfill : backfill;
        if (!subscription || subscription->cancellation.get_token() != token) return;
        subscription->afterRowId = nextRowId;
        subscription->atEnd = !hasMore;
        subscription->batchFailed = false;
        subscription->pending = admitted;
        if (admitted == 0)
        {
            if (subscription->atEnd) subscription->afterRowId = 0;
            subscription->nextScan = std::chrono::steady_clock::now() +
                (subscription->atEnd ? backfillIdleInterval : backfillBatchInterval);
        }
        else subscription->nextScan = std::chrono::steady_clock::time_point::max();
    }
    if (!missing.empty())
        Report("[MemoryAgent] backfill_scan | store=" + std::string(scan.archive ? "archive" : "curated") +
            " | rows=" + std::to_string(missing.size()) +
            " | queued=" + std::to_string(admitted));
}

std::tuple<const memory::ConversationArchive*, std::string, std::string> MemoryAgent::EmbeddingKey(const Task& task)
{
    return {task.archive, task.embeddingModel, task.memoryId};
}

void MemoryAgent::FinishEmbeddingTask(const Task& task, bool failed)
{
    std::lock_guard lock(mutex);
    pendingEmbeddingIds.erase(EmbeddingKey(task));
    auto& subscription = task.archive ? archiveBackfill : backfill;
    if (!task.scheduledBackfill || !subscription ||
        subscription->cancellation.get_token() != task.backfillStop) return;
    subscription->batchFailed = subscription->batchFailed || failed;
    if (subscription->pending > 0) --subscription->pending;
    if (subscription->pending != 0) return;
    subscription->failures = subscription->batchFailed ? std::min(subscription->failures + 1, 5) : 0;
    auto delay = subscription->atEnd ? backfillIdleInterval : backfillBatchInterval;
    if (subscription->batchFailed)
        delay = std::max(delay, BackfillRetryDelay(subscription->failures));
    if (subscription->atEnd)
        subscription->afterRowId = 0;
    subscription->nextScan = std::chrono::steady_clock::now() + delay;
}

std::vector<MemoryAgentEvent> MemoryAgent::DrainEvents()
{
    std::vector<PendingEvent> pending;
    {
        std::lock_guard lock(mutex);
        pending.swap(events);
    }
    std::vector<MemoryAgentEvent> drained;
    for (auto& event : pending)
    {
        if (Admitted(event.admission))
            drained.push_back(std::move(static_cast<MemoryAgentEvent&>(event)));
    }
    return drained;
}

void MemoryAgent::RetireUnadmittedSubscriptions()
{
    std::optional<BackfillSubscription> curated, archived;
    {
        std::lock_guard lock(mutex);
        curated = backfill;
        archived = archiveBackfill;
    }
    const bool retireCurated = curated && !Admitted(curated->admission);
    const bool retireArchive = archived && !Admitted(archived->admission);
    {
        std::lock_guard lock(mutex);
        // A concurrent refresh may have installed a newer guard. Retire only the
        // generation checked outside this lock, never its replacement.
        if (retireCurated && backfill && backfill->cancellation.get_token() == curated->cancellation.get_token()) backfill.reset();
        if (retireArchive && archiveBackfill && archiveBackfill->cancellation.get_token() == archived->cancellation.get_token()) archiveBackfill.reset();
    }
    if (retireCurated) curated->cancellation.request_stop();
    if (retireArchive) archived->cancellation.request_stop();
}

void MemoryAgent::Stop()
{
    DisableSemanticDuplicates();
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
    archiveBackfill.reset();
}

void MemoryAgent::Run(const std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        RetireUnadmittedSubscriptions();
        Task task;
        bool scanDue = false;
        {
            std::unique_lock lock(mutex);
            // A held-back turn has its own wake time. A newly queued turn changes it, so
            // the wait also ends when the interactive queue changes and is recomputed.
            const auto now = std::chrono::steady_clock::now();
            auto interactiveWake = std::chrono::steady_clock::time_point::max();
            const bool interactiveDue = InteractiveDueLocked(now, interactiveWake);
            const std::size_t interactiveSeen = interactiveTasks.size();
            const auto ready = [&]()
            {
                return interactiveDue || interactiveTasks.size() != interactiveSeen || !learningTasks.empty() || !backfillTasks.empty() ||
                       BackfillDueLocked();
            };
            // A profile change can replace the subscription while the wait releases the
            // mutex; its deadline must remain a local value.
            const auto deadline = std::min({backfill ? backfill->nextScan : std::chrono::steady_clock::time_point::max(),
                archiveBackfill ? archiveBackfill->nextScan : std::chrono::steady_clock::time_point::max(), interactiveWake});
            if (deadline != std::chrono::steady_clock::time_point::max())
                taskAvailable.wait_until(lock, stopToken, deadline, ready);
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
            auto unusedWake = std::chrono::steady_clock::time_point::max();
            const QueueDepths depths{
                InteractiveDueLocked(std::chrono::steady_clock::now(), unusedWake) ? interactiveTasks.size() : 0U, learningTasks.size(),
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

        if (!Admitted(task.admission))
        {
            FinishEmbeddingTask(task, false);
            continue;
        }
        std::stop_source requestStop;
        std::stop_callback ownerStopped(stopToken, [&requestStop] { requestStop.request_stop(); });
        std::stop_callback backfillStopped(task.backfillStop, [&requestStop] { requestStop.request_stop(); });
        const auto requestToken = requestStop.get_token();

        if (!task.memoryId.empty() && !task.hasLearnedDecision)
        {
            const auto needsEmbedding = [&]()
            {
                return task.archive ? task.archive->NeedsEmbedding(task.memoryId, task.embeddingModel) :
                    memory.NeedsEmbedding(task.memoryId, task.embeddingModel);
            };
            if (requestToken.stop_requested() ||
                !needsEmbedding())
            {
                FinishEmbeddingTask(task, false);
                continue;
            }
            const embeddingOutput embedding =
                task.router->EmbedMemory(task.input, requestToken);
            if (requestToken.stop_requested() || !Admitted(task.admission))
            {
                FinishEmbeddingTask(task, false);
                continue;
            }
            MemoryAgentEvent event;
            event.operation = task.archive ? "archive_backfill" : "memory_backfill";
            event.decision.bSuccess = embedding.bSuccess && embedding.model == task.embeddingModel;
            event.decision.reason = embedding.bSuccess && !event.decision.bSuccess
                ? "The embedding model changed while backfill was pending." : embedding.reason;
            event.decision.timings.push_back({
                "memory_document_embedding",
                embedding.elapsedMilliseconds});
            if (event.decision.bSuccess &&
                needsEmbedding() &&
                !requestToken.stop_requested() && Admitted(task.admission))
            {
                const auto saveStarted = std::chrono::steady_clock::now();
                event.saveSucceeded = task.archive ?
                    task.archive->SaveEmbedding(task.archivedTurn, embedding.model, embedding.values,
                        [&]() { return !requestToken.stop_requested() && Admitted(task.admission); }) :
                    memory.SaveEmbedding(task.memoryId, embedding.model, embedding.values);
                event.decision.timings.push_back({
                    "memory_db_save",
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - saveStarted).count()});
            }
            FinishEmbeddingTask(task, !event.decision.bSuccess || !event.saveSucceeded);
            if (!task.archive && event.decision.bSuccess && event.saveSucceeded)
                event.semanticDuplicateMarks = MarkSemanticDuplicates(task.memoryId, task.embeddingModel, task, requestToken);
            if (!Admitted(task.admission)) continue;
            std::string pressureReport;
            {
                std::lock_guard lock(mutex);
                pressureReport = AdmitEventLocked(std::move(event), task.admission);
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
                if (embedding.bSuccess && !requestToken.stop_requested() && Admitted(task.admission) &&
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
            if (event.embeddingError.empty())
                event.semanticDuplicateMarks = MarkSemanticDuplicates(task.memoryId, task.embeddingModel, task, requestToken);
        }
        else
        {
            std::function<memoryDecision(const std::string&, const std::string&)> evaluator;
            {
                std::lock_guard lock(mutex);
                evaluator = evaluateOverride;
            }
            event.decision = evaluator
                                 ? evaluator(task.input, task.assistantResponse)
                                 : task.router->EvaluateMemory(task.input, task.assistantResponse, task.provenance, stopToken, task.scope);
        }
        if (stopToken.stop_requested() || !Admitted(task.admission))
        {
            if (stopToken.stop_requested()) return;
            continue;
        }

        // A preempted evaluation reached no verdict at all, and nothing else retries it,
        // so letting it fall through here loses the turn's memory candidate outright --
        // the one outcome that looks identical to "not worth remembering". It goes back
        // on the queue behind fresher turns rather than ahead of them, because a newer
        // turn describes the conversation that is actually happening. Sustained pressure
        // still sheds it: the queue keeps its own bound, and the retry count is small.
        if (!task.hasLearnedDecision && event.decision.bPreempted &&
            task.preemptionRetries < maximumPreemptionRetries)
        {
            ++task.preemptionRetries;
            bool requeued = false;
            {
                std::lock_guard lock(mutex);
                if (interactiveTasks.size() < limits.maximumInteractive)
                {
                    interactiveTasks.push_back(std::move(task));
                    requeued = true;
                }
            }
            if (requeued)
            {
                taskAvailable.notify_one();
                continue;
            }
        }

        if (!task.hasLearnedDecision && event.decision.bSuccess && event.decision.bShouldRemember)
        {
            if (event.decision.subject.kind == memory::MemorySubjectKind::Participant)
                event.decision.subject = memory::ParticipantSubject(task.scope);
            else if (event.decision.subject.kind == memory::MemorySubjectKind::Companion)
                event.decision.subject = MayExpressOwnOpinion(task.provenance) && event.decision.category.starts_with("self_") &&
                                                 memory::IsAttributedPrivateScope(task.scope)
                                             ? memory::MemorySubject{memory::MemorySubjectKind::Companion, task.scope.companionId}
                                             : memory::MemorySubject{};
            const auto saveStarted = std::chrono::steady_clock::now();
            if (!Admitted(task.admission)) continue;
            std::string savedId;
            event.saveSucceeded = memory.SaveAutomaticMemory(event.decision, event.wasAdded, &savedId, task.admission);
            event.decision.timings.push_back({
                "memory_db_save",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - saveStarted).count()});
            if (event.saveSucceeded && event.wasAdded)
                event.semanticDuplicateMarks = MarkSemanticDuplicates(savedId, event.decision.embeddingModel, task, requestToken);
            if (event.saveSucceeded && task.provenance == ResponseProvenance::NormalGeneration)
                event.correctionProposals = memory.ProposeCorrections(savedId, event.decision, task.input,
                    task.scope, task.turnId, task.admission, requestToken);
        }

        if (!Admitted(task.admission)) continue;
        std::string pressureReport;
        {
            std::lock_guard lock(mutex);
            pressureReport = AdmitEventLocked(std::move(event), task.admission);
        }
        if (!pressureReport.empty()) Report(pressureReport);
    }
}

} // namespace revia::agents
