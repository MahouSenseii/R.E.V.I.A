#include "Audit/runtimeEvidenceBridge.h"
#include "Audit/contentDigest.h"

namespace revia::audit
{
struct RuntimeEvidenceBridge::State
{
    std::mutex mutex;
    std::condition_variable_any condition;
    std::shared_ptr<EvidenceJournal> journal;
    bool active = true;
};

RuntimeEvidenceBridge::~RuntimeEvidenceBridge()
{
    Stop();
}

void RuntimeEvidenceBridge::Start(runtime::RuntimeEventBus& inputBus, std::shared_ptr<EvidenceJournal> journal, DiagnosticSink diagnostic)
{
    Stop();
    if (!journal)
        return;
    bus = &inputBus;
    state = std::make_shared<State>();
    state->journal = std::move(journal);
    subscription = bus->Subscribe(
        [captured = state](const runtime::RuntimeEvent& event)
        {
            if (event.kind != runtime::RuntimeEventKind::StateChanged && event.kind != runtime::RuntimeEventKind::ComponentStatus &&
                event.kind != runtime::RuntimeEventKind::Timing && event.kind != runtime::RuntimeEventKind::Warning)
                return;
            if (event.stamp.companionId.empty() || event.stamp.sessionId.empty())
                return;
            JournalEvent observation;
            observation.evidence.id = NewJournalEventId();
            observation.evidence.sourceLocator = "runtime-event://" + observation.evidence.id;
            observation.evidence.sourceId = "runtime-lifecycle";
            observation.evidence.mediaType = "text/plain";
            observation.evidence.stamp = event.stamp;
            if (observation.evidence.stamp.taskId.empty())
                observation.evidence.stamp.taskId = "runtime-observations";
            if (observation.evidence.stamp.attemptId.empty())
                observation.evidence.stamp.attemptId = "lifecycle";
            observation.evidence.scope.companionId = event.stamp.companionId;
            observation.evidence.scope.audience = {identity::AudienceKind::Unknown, "runtime-system", 0, {}};
            const auto observed = std::chrono::duration_cast<std::chrono::milliseconds>(event.occurredAt.time_since_epoch()).count();
            if (observed <= 0)
                return;
            observation.evidence.observedAtUnixMs = static_cast<std::uint64_t>(observed);
            observation.redactedSummary = "Runtime kind " + std::to_string(static_cast<int>(event.kind)) + ", state " +
                                          runtime::ToString(event.state) + ". Content omitted.";
            observation.evidence.digest = ContentDigest(observation.redactedSummary);
            std::lock_guard lock(captured->mutex);
            if (captured->active)
                (void)captured->journal->QueueTelemetry(observation);
        });
    worker = std::jthread(
        [captured = state, diagnostic = std::move(diagnostic)](std::stop_token stop)
        {
            std::string prior;
            while (!stop.stop_requested())
            {
                (void)captured->journal->FlushTelemetry();
                const auto health = captured->journal->Health();
                const auto status = std::to_string(health.degraded) + ":" + health.error + ":" +
                                    std::to_string(health.droppedTelemetry != 0) + ":" +
                                    std::to_string(!health.unresolvedTransactions.empty());
                if (diagnostic && status != prior)
                {
                    prior = status;
                    try
                    {
                        diagnostic(health);
                    }
                    catch (...)
                    {
                        // Diagnostic listeners cannot terminate the persistence worker.
                    }
                }
                std::unique_lock lock(captured->mutex);
                captured->condition.wait_for(lock, stop, std::chrono::milliseconds(250), [] { return false; });
            }
            (void)captured->journal->FlushTelemetry();
        });
}

void RuntimeEvidenceBridge::Stop()
{
    if (state)
    {
        std::lock_guard lock(state->mutex);
        state->active = false;
    }
    if (bus && subscription != 0)
        bus->Unsubscribe(subscription);
    subscription = 0;
    bus = nullptr;
    worker.request_stop();
    if (state)
        state->condition.notify_all();
    if (worker.joinable())
        worker.join();
    state.reset();
}
}
