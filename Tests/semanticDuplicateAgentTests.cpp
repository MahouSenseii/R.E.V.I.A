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
    MemoryScope scope;
    scope.participantId = "duplicate-owner";
    scope.companionId = "duplicate-companion";
    scope.audience = {revia::identity::AudienceKind::Private, "duplicate-private", 1, {"duplicate-owner"}};
    scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    return scope;
}
memoryDecision Fact(const std::string& summary)
{
    memoryDecision value;
    value.bSuccess = value.bShouldRemember = true;
    value.summary = summary;
    value.subject = ParticipantSubject(Scope());
    value.embedding = {1,0};
    value.embeddingModel = "fixture-embedding";
    return value;
}
SemanticDuplicateSettings Settings()
{
    SemanticDuplicateSettings policy;
    policy.enabled = true;
    policy.modelIdentity = "fixture-nli";
    return policy;
}
class Scorer : public SemanticDuplicateScorer
{
public:
    std::atomic<int> calls = 0;
    std::atomic<bool> block = false;
    std::atomic<bool> sawCancellation = false;
    NliPairResult Compare(const std::string&, const std::string&, std::stop_token token) override
    {
        ++calls;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (block && !token.stop_requested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(2ms);
        sawCancellation = token.stop_requested();
        return {true, false, "fixture-nli", .995, .995, .002, .002};
    }
};
template<class Predicate> bool Until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do { if (predicate()) return true; std::this_thread::sleep_for(2ms); } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
void TestActualInteractiveWorkerMarksAfterSave()
{
    revia::tests::ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    longTermMemory store(path);
    bool added;
    Check(store.Save(Fact("The user prefers concise answers."), added), "Worker duplicate seed failed.");
    messageRouter router;
    MemoryAgent agent(path);
    MemoryDeferral deferral; deferral.settle = 0ms; deferral.maximum = 0ms;
    agent.SetDeferral(deferral);
    auto scorer = std::make_shared<Scorer>();
    agent.ConfigureSemanticDuplicates(Settings(), scorer, Scope(), [] { return true; });
    MemoryAgentTestAccess::SetEvaluator(agent, [](const auto&, const auto&) { return Fact("The user likes short replies."); });
    agent.Submit(router, "I prefer short replies.", "Understood.", ResponseProvenance::NormalGeneration, 1, [] { return true; }, Scope());
    Check(Until([&] { return store.LoadSemanticDuplicateMarks(Scope()).size() == 1; }), "Actual worker did not mark the saved fact.");
    agent.Stop();
    const auto events = agent.DrainEvents();
    Check(events.size() == 1 && events.front().wasAdded && events.front().saveSucceeded &&
        events.front().semanticDuplicateMarks == 1 && store.Load().size() == 2, "Worker advisory result changed durable save disposition.");
}
void TestConfiguredScopeAndCancellation()
{
    for (int mode = 0; mode < 4; ++mode)
    {
        revia::tests::ScopedTestDirectory directory;
        const auto path = (directory.root / "memory.db").string();
        longTermMemory store(path);
        bool added; std::string id;
        Check(store.Save(Fact("The user prefers concise answers."), added) &&
            store.Save(Fact("The user likes short replies."), added, &id), "Cancellation fixture seed failed.");
        MemoryAgent agent(path);
        auto scorer = std::make_shared<Scorer>();
        agent.ConfigureSemanticDuplicates(Settings(), scorer, Scope(), [] { return true; });
        auto denied = Scope(); denied.audience.kind = revia::identity::AudienceKind::Public;
        Check(MemoryAgentTestAccess::MarkDuplicates(agent, id, denied) == 0 && scorer->calls == 0,
            "Public captured task borrowed a private configuration.");
        denied = Scope(); denied.participantId = "different-owner";
        Check(MemoryAgentTestAccess::MarkDuplicates(agent, id, denied) == 0 && scorer->calls == 0,
            "Another captured subject borrowed private configuration.");
        Check(MemoryAgentTestAccess::MarkDuplicates(agent, id, Scope(), [] { return false; }) == 0 && scorer->calls == 0,
            "Retired task reached scorer through live configuration.");
        scorer->block = true;
        auto pending = std::async(std::launch::async, [&] { return MemoryAgentTestAccess::MarkDuplicates(agent, id, Scope()); });
        Check(Until([&] { return scorer->calls == 1; }), "Cancellation fixture did not reach scorer.");
        if (mode == 0) agent.DisableSemanticDuplicates();
        if (mode == 1) agent.ConfigureSemanticDuplicates(Settings(), scorer, Scope(), [] { return true; });
        if (mode == 2) agent.Stop();
        if (mode == 3)
        {
            auto replacement = Scope(); replacement.participantId = "different-owner";
            agent.ConfigureSemanticDuplicates(Settings(), scorer, replacement, [] { return true; });
        }
        Check(pending.get() == 0 && scorer->sawCancellation && store.LoadSemanticDuplicateMarks(Scope()).empty() &&
            store.Load().size() == 2, "Retired configuration persisted metadata or lost facts.");
        agent.Stop();
    }
}
}

void RunSemanticDuplicateAgentTests()
{
    TestActualInteractiveWorkerMarksAfterSave();
    TestConfiguredScopeAndCancellation();
    std::cout << "Semantic duplicate worker save, scope, configuration retirement and stop checks passed.\n";
}
