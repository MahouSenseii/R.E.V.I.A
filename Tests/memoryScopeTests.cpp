#include "testSupport.h"
#include "Audit/contentDigest.h"
#include "Core/conversationContext.h"
#include "Memory/conversationArchive.h"
#include "Memory/longTermMemory.h"
#include "Memory/memoryScope.h"

#include <algorithm>
#include <sqlite3.h>

namespace
{
using revia::tests::Check;
using namespace revia::memory;

MemoryScope Scope(const std::string& participant)
{
    MemoryScope scope;
    scope.companionId = "fixture-companion";
    scope.participantId = participant;
    scope.audience = {revia::identity::AudienceKind::Private, "local-private", 7, {participant}};
    scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    return scope;
}

memoryDecision Decision(const std::string& text, const std::string& participant)
{
    memoryDecision decision;
    decision.bSuccess = decision.bShouldRemember = true;
    decision.category = "preference";
    decision.summary = text;
    if (!participant.empty())
        decision.subject = {MemorySubjectKind::Participant, participant};
    return decision;
}

void TestLegacyMigrationRemainsUnattributed()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = (fixture.root / "legacy.db").string();
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Legacy fixture open failed.");
    const int result = sqlite3_exec(database,
        "CREATE TABLE memories(id TEXT NOT NULL UNIQUE,category TEXT NOT NULL,summary TEXT NOT NULL,"
        "normalized_summary TEXT NOT NULL UNIQUE,source TEXT NOT NULL,created_at TEXT NOT NULL,active INTEGER NOT NULL DEFAULT 1);"
        "INSERT INTO memories VALUES('legacy','preference','Prefers mint tea.','v2:prefers mint tea.','automatic','100',1);",
        nullptr, nullptr, nullptr);
    sqlite3_close(database);
    Check(result == SQLITE_OK, "Legacy fixture schema failed.");
    const auto alice = Scope("local:alice");
    for (int pass = 0; pass < 2; ++pass)
    {
        longTermMemory migrated(path);
        Check(migrated.Load().size() == 1 && migrated.Load().front().subject == MemorySubject{}, "Migration guessed a legacy subject.");
        Check(migrated.Search("mint", 6, {}, "", 0, &alice).empty(), "Legacy row entered automatic participant recall.");
        bool added = true;
        std::string id;
        Check(migrated.Save(Decision("Prefers mint tea.", ""), added, &id) && !added && id == "legacy",
            "Migration lost old-key deduplication.");
    }
}

void TestSubjectIsolationAndRevision()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = (fixture.root / "scope.db").string();
    longTermMemory store(path);
    const auto alice = Scope("person-alice"), bob = Scope("person-bob");
    bool added = false;
    std::string first, second, legacy, error;
    Check(store.Save(Decision("Prefers mint tea.", alice.participantId), added, &first) && added, "Alice save failed.");
    Check(store.Save(Decision("Prefers mint tea.", bob.participantId), added, &second) && added && second != first,
        "Equal summaries from distinct participants merged or collided in one second.");
    Check(store.Save(Decision("Prefers mint tea.", ""), added, &legacy) && added && legacy != first,
        "Unattributed content inherited a participant.");
    Check(store.Search("mint", 1, {}, "", 0, &alice).size() == 1 && store.Search("mint", 1, {}, "", 0, &alice).front().id == first,
        "Private recall crossed participant or legacy scope.");
    Check(store.Search("mint", 1, {}, "", 0, &bob).front().id == second, "Bob's recall lost his equal summary.");
    auto opinion = Decision("Revia prefers thunderstorms.", "");
    opinion.category = "self_preference";
    opinion.subject = {MemorySubjectKind::Companion, alice.companionId};
    Check(store.Save(opinion, added) && added, "Companion opinion save failed.");
    opinion.summary = "Another companion prefers thunderstorms indoors.";
    opinion.subject.entityId = "other-companion";
    Check(store.Save(opinion, added) && added, "Other companion opinion save failed.");
    const auto opinions = store.Search("thunderstorms", 6, {}, "", 0, &alice);
    Check(opinions.size() == 1 && opinions.front().subject.entityId == alice.companionId,
        "Scoped recall lost its own companion belief or exposed another companion's belief.");
    auto unknown = Scope("");
    Check(store.Search("mint", 1, {}, "", 0, &unknown).empty(), "Unknown participant recalled private facts.");
    auto shared = alice;
    shared.audience.kind = revia::identity::AudienceKind::Shared;
    Check(store.BuildPromptBlock("mint", 6, {}, "", 0, &shared).empty(), "Shared audience recalled private facts.");
    for (int index = 0; index < 30; ++index)
        Check(store.Save(Decision("Mint tea variation " + std::to_string(index), bob.participantId), added), "Candidate save failed.");
    Check(store.Search("mint", 1, {}, "", 0, &alice).front().id == first, "Scope was filtered after the candidate limit.");
    const auto scoped = store.LoadScoped(alice, 12);
    Check(scoped.size() == 2 && std::all_of(scoped.begin(), scoped.end(),
                                    [&](const auto& entry)
                                    {
                                        return entry.subject == ParticipantSubject(alice) ||
                                               entry.subject == MemorySubject{MemorySubjectKind::Companion, alice.companionId};
                                    }),
        "Scoped inventory lost an older matching row or included other/legacy subjects before its limit.");
    Check(store.LoadScoped(alice, 1).size() == 1 && store.LoadScoped(shared, 12).empty() && store.LoadScoped(unknown, 12).empty() &&
              store.LoadScoped(alice, 0).empty(),
        "Scoped inventory did not enforce scope and row bounds.");
    Check(store.SaveEmbedding(first, "scope-model", {1.0F, 0.0F}) && store.SaveEmbedding(second, "scope-model", {1.0F, 0.0F}),
        "Scope vector setup failed.");
    Check(store.Search("", 1, {1.0F, 0.0F}, "scope-model", 0, &alice).front().id == first, "Semantic recall crossed participant scope.");

    MemoryRevisionRequest request;
    request.ownerRequestId = "scope-revision";
    request.originalId = first;
    request.expectedSummaryDigest = revia::audit::ContentDigest("Prefers mint tea.");
    request.expectedSubject = {MemorySubjectKind::Participant, bob.participantId};
    request.origin = {"companion", "session", 1, {}, {}, 1};
    request.audienceRevision = 7;
    request.corrected = Decision("Prefers ginger tea now.", alice.participantId);
    request.reason = "Owner explicitly corrected the selected fact.";
    request.evidence = "Selected exact record and supplied correction.";
    MemoryRevisionReceipt receipt, replay;
    Check(!store.SaveOwnerRevision(request, receipt, error), "Revision accepted another participant's target.");
    request.expectedSubject = request.corrected.subject;
    Check(store.SaveOwnerRevision(request, receipt, error) && receipt.subject == request.expectedSubject, error);
    Check(store.SaveOwnerRevision(request, replay, error) && replay.revisedId == receipt.revisedId, "Scoped revision replay changed.");
    longTermMemory reopened(path);
    const auto block = reopened.BuildPromptBlock("mint", 1, {}, "", 0, &alice);
    Check(block.find("ginger") != std::string::npos && reopened.Load().front().subject == request.expectedSubject,
        "Restart lost subject or receipt-linked correction.");
    const auto before = store.Load().size();
    int admissions = 0;
    Check(!store.Save(
              Decision("Rejected during durable admission.", alice.participantId), added, nullptr, [&] { return ++admissions == 1; }) &&
              store.Load().size() == before,
        "Retired automatic save committed content.");
}

void TestScopedArchiveRestart()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = (fixture.root / "archive.db").string();
    const auto alice = Scope("person-alice"), bob = Scope("person-bob");
    std::string error;
    {
        ConversationArchive archive(path);
        Check(archive.BeginSession("older-mixed", error), error);
        Check(archive.Record("older-mixed", "user", "We decided the launch stays on Friday.", error, alice), error);
        Check(archive.Record("older-mixed", "user", "Bob's hidden launch is Monday.", error, bob), error);
        Check(archive.Record("older-mixed", "user", "Unattributed launch is Sunday.", error), error);
        Check(archive.Record("older-mixed", "user", std::string(9000, 'x') + " Actually the launch is Tuesday.", error, alice), error);
        Check(archive.Record("older-mixed", "assistant", "Tuesday is the current date.", error, alice), error);
        Check(archive.BeginSession("newer-bob", error), error);
        Check(archive.Record("newer-bob", "user", "Bob latest launch secret.", error, bob), error);
        int admissions = 0;
        Check(!archive.Record("older-mixed", "user", "Retired scope content.", error, alice, [&] { return ++admissions == 1; }),
            "Retired archive write committed content.");
    }
    ConversationArchive reopened(path);
    const auto restored = reopened.LoadPreviousSessionTail("current", 500, &alice);
    Check(restored.size() == 3 && restored.front().sessionId == "older-mixed", "Restart selected another participant's session.");
    Check(restored[1].content.find("Actually the launch is Tuesday") != std::string::npos, "Archive truncation lost late correction.");
    for (const auto& turn : restored)
        Check(turn.scope.participantId == alice.participantId, "Archive mixed participant or unknown provenance.");
    const auto matching = reopened.SearchRange({"launch"}, 0, 9999999999LL, 20, &alice);
    Check(matching.size() == 2, "Archive lexical scope failed.");
    Check(reopened.LoadRange(0, 9999999999LL, 20, &alice).size() == 3, "Archive temporal scope failed.");
    Check(reopened.SearchEarliest({"launch"}, 1, &alice).front().content.find("Friday") != std::string::npos,
        "Earliest-mention scope failed.");
    auto otherCompanion = alice;
    otherCompanion.companionId = "other-companion";
    Check(reopened.LoadPreviousSessionTail("current", 6, &otherCompanion).empty(), "Archive restored another companion's rows.");
    auto shared = alice;
    shared.audience.kind = revia::identity::AudienceKind::Shared;
    Check(reopened.LoadPreviousSessionTail("current", 6, &shared).empty(), "Shared context restored private archive.");
    auto voice = alice;
    voice.participantSource = revia::identity::SpeakerSource::ConsentedVoice;
    voice.consentRevision = 0;
    Check(!IsAttributedPrivateScope(voice), "Unconsented voice admitted private scope.");
    Check(reopened.BeginSession("current", error), error);
    Check(reopened.Record("current", "user", "Alice current launch note.", error, alice), error);
    Check(reopened.Record("current", "user", "Bob current launch note.", error, bob), error);
    const auto switchedBack = reopened.LoadLatestCompatibleTail("current", 500, alice);
    Check(switchedBack.size() == 1 && switchedBack.front().content == "Alice current launch note.",
        "Switching back to a participant ignored compatible current-session history.");
}
}

void RunMemoryScopeTests()
{
    TestSubjectIsolationAndRevision();
    TestScopedArchiveRestart();
    TestLegacyMigrationRemainsUnattributed();
}
