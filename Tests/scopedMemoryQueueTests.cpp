#include "testSupport.h"
#include "memoryAgentTestAccess.h"
#include "Core/messageRouter.h"
#include "Memory/longTermMemory.h"

#include <atomic>
#include <chrono>
#include <condition_variable>

void RunScopedMemoryQueueTests()
{
    using namespace revia;
    using tests::Check;
    tests::ScopedTestDirectory directory;
    const auto path = (directory.root / "queued.db").string();
    messageRouter router(path);
    memory::MemoryScope captured;
    captured.companionId = "fixture-companion";
    captured.participantId = "local:alice";
    captured.audience = {identity::AudienceKind::Private, "local-private", 1, {"local:alice"}};
    captured.participantSource = identity::SpeakerSource::ExplicitIntroduction;
    std::mutex mutex;
    std::condition_variable condition;
    bool evaluating = false, release = false, sentinel = false;
    std::atomic<bool> admitted = true;
    agents::MemoryAgent agent(path);
    struct Release
    {
        std::mutex& mutex;
        std::condition_variable& condition;
        bool& release;
        ~Release()
        {
            std::lock_guard lock(mutex);
            release = true;
            condition.notify_all();
        }
    } releaseOnFailure{mutex, condition, release};
    agents::MemoryAgentTestAccess::SetEvaluator(agent,
        [&](const std::string& input, const std::string&)
        {
            std::unique_lock lock(mutex);
            if (input == "sentinel")
            {
                sentinel = true;
                condition.notify_all();
                return memoryDecision{};
            }
            evaluating = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
            memoryDecision decision;
            decision.bSuccess = decision.bShouldRemember = true;
            decision.category = "preference";
            decision.summary = "The user prefers mint tea.";
            decision.subject = {memory::MemorySubjectKind::Participant, "provider-invented-id"};
            return decision;
        });
    agent.Submit(
        router, "I prefer mint tea.", "Understood.", agents::ResponseProvenance::NormalGeneration, 1, [&] { return admitted.load(); },
        captured);
    {
        std::unique_lock lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(3), [&] { return evaluating; }), "Scoped queue did not begin evaluation.");
        captured.participantId = "local:bob";
        release = true;
    }
    condition.notify_all();
    agent.Submit(router, "sentinel", "", agents::ResponseProvenance::NormalGeneration);
    {
        std::unique_lock lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(3), [&] { return sentinel; }), "Scoped queue did not complete prior task.");
    }
    auto rows = longTermMemory(path).Load();
    Check(rows.size() == 1 && rows.front().subject.entityId == "local:alice",
        "Queue read a mutable current identity or trusted a provider ID.");
    {
        std::lock_guard lock(mutex);
        evaluating = release = sentinel = false;
    }
    agent.Submit(
        router, "I prefer mint tea.", "Understood.", agents::ResponseProvenance::NormalGeneration, 2, [&] { return admitted.load(); },
        captured);
    {
        std::unique_lock lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(3), [&] { return evaluating; }), "Second scoped queue did not begin.");
        admitted = false;
        release = true;
    }
    condition.notify_all();
    agent.Submit(router, "sentinel", "", agents::ResponseProvenance::NormalGeneration);
    {
        std::unique_lock lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(3), [&] { return sentinel; }), "Revoked queue did not retire.");
    }
    memoryDecision lesson;
    lesson.bSuccess = lesson.bShouldRemember = true;
    lesson.category = "planning";
    lesson.summary = "A reviewed lesson belongs to the companion whose admission was captured.";
    lesson.subject = {memory::MemorySubjectKind::Companion, captured.companionId};
    std::string lessonId;
    Check(agent.SubmitLearnedFinding(router, lesson, 3, &lessonId, [] { return false; }) == agents::LearnedFindingResult::Failed &&
              lessonId.empty() && longTermMemory(path).Load().size() == 1,
        "Retired captured admission saved a newly reviewed lesson.");
    Check(agent.SubmitLearnedFinding(router, lesson, 3, &lessonId, [] { return true; }) != agents::LearnedFindingResult::Failed &&
              !lessonId.empty(),
        "Admitted companion lesson did not receive a durable memory receipt.");
    const auto admittedRows = longTermMemory(path).Load();
    Check(std::any_of(admittedRows.begin(), admittedRows.end(),
              [&](const auto& row) { return row.id == lessonId && row.subject == lesson.subject; }),
        "Accepted lesson lost its captured companion subject.");
    agent.Stop();
    Check(longTermMemory(path).Load().size() == 2, "Revoked queued scope saved another participant's fact.");
    const auto events = agent.DrainEvents();
    Check(
        std::none_of(events.begin(), events.end(), [](const auto& event) { return event.turnId == 2; }), "Revoked queue emitted an event.");
}
