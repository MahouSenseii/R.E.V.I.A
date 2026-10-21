#include "testSupport.h"
#include "memoryAgentTestAccess.h"
#include <atomic>
#include <future>
#include <iostream>

namespace
{
using namespace revia::memory;
using namespace revia::agents;
using revia::tests::Check;
using namespace std::chrono_literals;
MemoryScope Scope()
{
    MemoryScope value;
    value.participantId = "correction-worker-owner";
    value.companionId = "correction-worker-companion";
    value.audience = {revia::identity::AudienceKind::Private, "correction-worker-private", 1, {value.participantId}};
    value.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    return value;
}
memoryDecision Fact(const std::string& text)
{
    memoryDecision value;
    value.bSuccess = value.bShouldRemember = true;
    value.category = "identity";
    value.summary = text;
    value.subject = ParticipantSubject(Scope());
    return value;
}
template<class Predicate> bool Until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do { if (predicate()) return true; std::this_thread::sleep_for(2ms); } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
void TestOrdinaryWorkerSavesThenProposesWithoutRevision()
{
    for (int mode = 0; mode < 5; ++mode)
    {
        revia::tests::ScopedTestDirectory fixture;
        const auto path = (fixture.root / "memory.db").string();
        longTermMemory store(path);
        bool added; std::string oldId, newId;
        Check(store.Save(Fact("The user lives in Paris."), added, &oldId), "Correction worker seed failed.");
        const auto corrected = Fact("The user lives in Oslo.");
        if (mode == 1) Check(store.Save(corrected, added, &newId), "Correction repeat seed failed.");
        messageRouter router;
        MemoryAgent agent(path);
        MemoryDeferral deferral; deferral.settle = 0ms; deferral.maximum = 0ms; agent.SetDeferral(deferral);
        MemoryAgentTestAccess::SetEvaluator(agent, [&](const auto&, const auto&) { return corrected; });
        const auto input = mode == 2 ? "\"Actually I now live in Oslo.\"" : "Actually I now live in Oslo.";
        auto scope = Scope(); if (mode == 4) scope.audience.kind = revia::identity::AudienceKind::Public;
        agent.Submit(router, input, "Understood.", mode == 3 ? ResponseProvenance::Roleplay : ResponseProvenance::NormalGeneration,
            17, [] { return true; }, scope);
        std::vector<MemoryAgentEvent> events;
        Check(Until([&] { events = agent.DrainEvents(); return !events.empty(); }), "Correction worker produced no event.");
        agent.Stop();
        Check(events.size() == 1 && events.front().saveSucceeded && store.RevisionHistory().empty(),
            "Ordinary classification failed or created owner authority.");
        if (mode < 2)
        {
            const auto& event = events.front();
            Check(event.correctionProposals.size() == 1 && event.correctionProposals.front().original.id == oldId &&
                event.correctionProposals.front().corrected.summary == corrected.summary &&
                event.correctionProposals.front().turnId == 17 && store.Load().size() == 2,
                "Ordinary successful save did not publish exact review proposal.");
            Check(event.wasAdded == (mode == 0) && MemoryAgent::ClassifyEventPriority(event) == MemoryEventPriority::Important,
                "Existing replacement lost review event priority.");
        }
        else Check(events.front().correctionProposals.empty(), "Quoted, roleplay or public task produced a proposal.");
    }
}
void TestRetiredTaskCannotPublishCorrection()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = (fixture.root / "memory.db").string();
    longTermMemory store(path);
    bool added;
    Check(store.Save(Fact("The user lives in Paris."), added), "Retirement seed failed.");
    messageRouter router;
    MemoryAgent agent(path);
    MemoryDeferral deferral; deferral.settle = 0ms; deferral.maximum = 0ms; agent.SetDeferral(deferral);
    std::atomic<bool> entered = false, admitted = true;
    MemoryAgentTestAccess::SetEvaluator(agent, [&](const auto&, const auto&)
    {
        entered = true;
        admitted = false;
        return Fact("The user lives in Oslo.");
    });
    agent.Submit(router, "Actually I now live in Oslo.", "Understood.", ResponseProvenance::NormalGeneration, 18,
        [&] { return admitted.load(); }, Scope());
    Check(Until([&] { return entered.load(); }), "Retirement fixture never entered classifier.");
    agent.Stop();
    Check(agent.DrainEvents().empty() && store.Load().size() == 1 && store.RevisionHistory().empty(),
        "Retired classification published a correction or saved stale content.");
}
void TestBlockedAudienceGuardDoesNotOwnQueueMutex()
{
    revia::tests::ScopedTestDirectory fixture;
    MemoryAgent agent((fixture.root / "memory.db").string());
    agent.Stop(); // Only the explicit drain and owner-style capture run below.
    std::mutex audience;
    std::atomic<bool> armed = false;
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    MemoryAgentTestAccess::QueueEvent(agent, [&]
    {
        if (armed)
        {
            entered.set_value();
            const std::lock_guard scopeLock(audience);
        }
        return true;
    });
    std::unique_lock scopeLock(audience);
    armed = true;
    auto draining = std::async(std::launch::async, [&] { return agent.DrainEvents(); });
    const bool reached = enteredFuture.wait_for(1s) == std::future_status::ready;
    auto owner = std::async(std::launch::async, [&] { MemoryAgentTestAccess::CaptureGuard(agent); });
    const bool ownerProceeded = owner.wait_for(500ms) == std::future_status::ready;
    // Always release the simulated session scope before asserting, so the old
    // inversion fails the fixture cleanly instead of hanging its teardown.
    scopeLock.unlock();
    owner.get();
    const auto events = draining.get();
    Check(reached && ownerProceeded && events.size() == 1,
        "Audience admission held the queue mutex and blocked the owner revision path.");
}
}
void RunMemoryCorrectionAgentTests()
{
    TestOrdinaryWorkerSavesThenProposesWithoutRevision();
    TestRetiredTaskCannotPublishCorrection();
    TestBlockedAudienceGuardDoesNotOwnQueueMutex();
    std::cout << "Ordinary MemoryAgent correction proposal, exact repeat, provenance, scope and retired-task checks passed.\n";
}
