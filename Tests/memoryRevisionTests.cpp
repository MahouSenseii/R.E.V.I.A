#include "testSupport.h"
#include "Audit/contentDigest.h"
#include "Memory/longTermMemory.h"
#include "Memory/memoryReconciliation.h"

#include <iostream>
#include <atomic>
#include <barrier>
#include <thread>
#include <future>
#include <chrono>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

namespace
{
using revia::tests::Check;
using namespace revia::memory;

memoryDecision Decision(const std::string& summary)
{
    memoryDecision value;
    value.bSuccess = value.bShouldRemember = true;
    value.category = "project";
    value.summary = summary;
    return value;
}

MemoryRevisionRequest Request(const std::string& id, const std::string& summary)
{
    MemoryRevisionRequest value;
    value.ownerRequestId = "owner-revision-1";
    value.originalId = id;
    value.expectedSummaryDigest = revia::audit::ContentDigest(summary);
    value.origin = {"synthetic-companion", "synthetic-session", 1, {}, {}, 1};
    value.audienceRevision = 1;
    value.corrected = Decision("The owner tracks Amber at nine.");
    value.reason = "The owner explicitly requested replacing the selected fact.";
    value.evidence = "Selected exact record and supplied replacement text.";
    return value;
}

void TestAppendOnlyRevisionRestartAndReversion()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = (fixture.root / "memory.db").string();
    longTermMemory store(path);
    const auto original = Decision("The owner tracks Amber at eight.");
    bool added;
    std::string id, error;
    Check(store.Save(original, added, &id), "Original save failed.");
    Check(store.SaveEmbedding(id, "synthetic-model", {1.0F, 0.0F}), "Original vector save failed.");
    auto request = Request(id, original.summary);
    MemoryRevisionReceipt receipt, repeated;
    Check(store.SaveOwnerRevision(request, receipt, error), error);
    Check(receipt.originalId == id && receipt.revisedId != id && !receipt.createdAt.empty() && receipt.chainId == id &&
              store.Load().size() == 2,
        "Revision overwrote the original or lost exact provenance.");
    Check(store.SaveOwnerRevision(request, repeated, error) && repeated.requestId == receipt.requestId &&
              repeated.revisedId == receipt.revisedId && store.RevisionHistory().size() == 1,
        "Exact authorization replay created another revision.");
    auto changed = request;
    changed.evidence += " Changed authorization material.";
    Check(!store.SaveOwnerRevision(changed, repeated, error), "Changed payload reused a prior owner request.");
    longTermMemory restarted(path);
    const auto entries = restarted.Load();
    Check(entries.size() == 2 && entries.front().summary == original.summary && entries.front().currentRevisionId == receipt.revisedId &&
              entries.back().revisesMemoryId == id,
        "Restart lost historical/current revision projection or original text.");
    Check(!restarted.NeedsEmbedding(id, "synthetic-model"), "Revision discarded the original embedding.");
    const auto block = restarted.BuildPromptBlock("eight", 1);
    Check(block.find("historical owner-revised") != std::string::npos && block.find(request.corrected.summary) != std::string::npos &&
              block.find("owner_revision:") != std::string::npos,
        "Recall exposed an old fact without its explicit current replacement and provenance.");
    auto revert = Request(receipt.revisedId, request.corrected.summary);
    revert.ownerRequestId = "owner-revision-2";
    revert.priorReceiptId = receipt.requestId;
    revert.corrected = original;
    Check(restarted.SaveOwnerRevision(revert, repeated, error) && repeated.revisedId == id && !repeated.wasAdded,
        "Explicit reversion could not reuse retained content through an append-only receipt chain.");
    const auto reverted = restarted.Load();
    Check(reverted.front().currentRevisionId == id && reverted.back().currentRevisionId == id && restarted.RevisionHistory().size() == 2,
        "Explicit reversion created a cycle in current-record selection.");
}

void TestExactTargetsAdmissionAndStaleChains()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    bool added;
    std::string id, error;
    const auto original = Decision("The owner tracks Amber at eight.");
    Check(store.Save(original, added, &id), "Original save failed.");
    auto request = Request(id, original.summary);
    MemoryRevisionReceipt receipt;
    auto invalid = request;
    invalid.originalId = "missing-memory";
    Check(!store.SaveOwnerRevision(invalid, receipt, error), "Missing exact target was revised.");
    invalid = request;
    invalid.expectedSummaryDigest = revia::audit::ContentDigest("Different stored text");
    Check(!store.SaveOwnerRevision(invalid, receipt, error), "Mismatched expected target was revised.");
    invalid = request;
    invalid.evidence = "password=synthetic-secret-credential";
    Check(!store.SaveOwnerRevision(invalid, receipt, error), "Unsafe revision evidence was saved.");
    Check(!store.SaveOwnerRevision(request, receipt, error, [] { return false; }), "Retired origin persisted a revision.");
    Check(store.Load().size() == 1 && store.RevisionHistory().empty(), "Rejected revisions partially mutated memory.");
    int admissions = 0;
    Check(!store.SaveOwnerRevision(request, receipt, error, [&] { return ++admissions == 1; }) && store.Load().size() == 1 &&
              store.RevisionHistory().empty(),
        "Origin retirement before commit retained correction content.");
    Check(store.SaveOwnerRevision(request, receipt, error), error);
    request.ownerRequestId = "owner-revision-stale";
    Check(!store.SaveOwnerRevision(request, receipt, error), "An old chain target was silently revised again.");
    Check(ClassifyRelation(original.summary, "The owner no longer tracks Amber at eight.", 1.0F) == MemoryRelation::Contradiction,
        "Diagnostic relation fixture did not retain its invariant.");
    Check(store.Save(Decision("The owner no longer tracks Amber at eight."), added) && added && store.RevisionHistory().size() == 1,
        "Similarity or classifier created an unauthorized revision.");
}

void TestReceiptFailureRollsBackCorrection()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = (fixture.root / "memory.db").string();
    longTermMemory store(path);
    bool added;
    std::string id, error;
    const auto original = Decision("The owner tracks Amber at eight.");
    Check(store.Save(original, added, &id), "Original save failed.");
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Could not inspect rollback fixture.");
    Check(sqlite3_exec(database,
              "CREATE TRIGGER fixture_receipt_failure BEFORE INSERT ON memory_revisions "
              "BEGIN SELECT RAISE(ABORT, 'fixture storage failure'); END;",
              nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not install receipt failure fixture.");
    MemoryRevisionReceipt receipt;
    Check(!store.SaveOwnerRevision(Request(id, original.summary), receipt, error) && receipt.requestId.empty(),
        "Failed revision receipt reported success.");
    Check(store.Load().size() == 1 && store.RevisionHistory().empty(), "Receipt failure retained an unreceipted correction.");
    sqlite3_close(database);
}

void TestConcurrentExplicitRequestsRetainOneCurrentChain()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = (fixture.root / "memory.db").string();
    longTermMemory first(path), second(path);
    bool added;
    std::string id;
    const auto original = Decision("The owner tracks Amber at eight.");
    Check(first.Save(original, added, &id), "Original concurrency save failed.");
    second.HasMemories();
    std::barrier start(2);
    std::atomic<int> saved = 0;
    const auto revise = [&](longTermMemory& store, const std::string& requestId)
    {
        auto request = Request(id, original.summary);
        request.ownerRequestId = requestId;
        MemoryRevisionReceipt receipt;
        std::string error;
        start.arrive_and_wait();
        if (store.SaveOwnerRevision(request, receipt, error))
            ++saved;
    };
    std::jthread a([&] { revise(first, "concurrent-owner-a"); });
    std::jthread b([&] { revise(second, "concurrent-owner-b"); });
    a.join();
    b.join();
    Check(saved == 1 && first.RevisionHistory().size() == 1 && first.Load().size() == 2,
        "Concurrent exact-target requests created competing current revisions or unreceipted content.");
}

void TestLegacyDigestAndExactSavedReplacement()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    bool added; std::string id, replacement, error;
    const auto original = Decision("The owner tracks Amber at eight.");
    Check(store.Save(original, added, &id), "Legacy digest seed failed.");
    auto legacy = Request(id, original.summary);
    const nlohmann::json previousRepresentation = {{"ownerRequestId", legacy.ownerRequestId}, {"originalId", legacy.originalId},
        {"expectedSummaryDigest", legacy.expectedSummaryDigest}, {"priorReceiptId", legacy.priorReceiptId},
        {"origin", {{"companionId", legacy.origin.companionId}, {"sessionId", legacy.origin.sessionId}, {"generation", legacy.origin.generation},
            {"taskId", legacy.origin.taskId}, {"attemptId", legacy.origin.attemptId}, {"policyVersion", legacy.origin.policyVersion}}},
        {"audienceRevision", legacy.audienceRevision}, {"category", legacy.corrected.category}, {"summary", legacy.corrected.summary},
        {"reason", legacy.reason}, {"evidence", legacy.evidence}};
    MemoryRevisionReceipt receipt, repeated;
    Check(store.SaveOwnerRevision(legacy, receipt, error) && receipt.digest == revia::audit::ContentDigest(previousRepresentation.dump()),
        "Empty replacement guard changed the legacy request digest.");
    Check(store.SaveOwnerRevision(legacy, repeated, error) && repeated.digest == receipt.digest, "Legacy receipt replay failed.");
    legacy.expectedReplacementId = receipt.revisedId;
    Check(!store.SaveOwnerRevision(legacy, repeated, error), "Changed replacement guard reused legacy authorization.");

    auto revert = Request(receipt.revisedId, legacy.corrected.summary);
    revert.ownerRequestId = "exact-existing-reversion";
    revert.priorReceiptId = receipt.requestId;
    revert.corrected = original;
    revert.expectedReplacementId = id;
    Check(store.SaveOwnerRevision(revert, repeated, error) && repeated.revisedId == id && !repeated.wasAdded && store.Load().size() == 2,
        "Exact saved replacement could not reuse an existing historical fact through its own chain.");
    Check(store.SaveOwnerRevision(revert, receipt, error) && receipt.digest == repeated.digest && store.RevisionHistory().size() == 2,
        "Exact replacement authorization replay created another receipt.");
    revert.expectedReplacementId = "different-target";
    Check(!store.SaveOwnerRevision(revert, receipt, error), "Changed exact target reused accepted authorization.");
}

void TestStaleReplacementRollsBackWithoutThirdFact()
{
    for (int mode = 0; mode < 6; ++mode)
    {
        revia::tests::ScopedTestDirectory fixture;
        const auto path = (fixture.root / "memory.db").string();
        longTermMemory store(path);
        bool added; std::string oldId, newId, error;
        const auto original = Decision("The owner tracks Amber at eight.");
        Check(store.Save(original, added, &oldId), "Replacement guard original seed failed.");
        auto request = Request(oldId, original.summary);
        Check(store.Save(request.corrected, added, &newId), "Replacement guard new seed failed.");
        request.expectedReplacementId = newId;
        sqlite3* db = nullptr;
        Check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "Could not mutate replacement fixture.");
        if (mode == 0) request.expectedReplacementId = "missing-replacement";
        else
        {
            const char* sql = mode == 1 ? "UPDATE memories SET active = 0 WHERE id = ?;" :
                mode == 2 ? "UPDATE memories SET summary = 'The owner tracks Amber at ten.' WHERE id = ?;" :
                mode == 3 ? "UPDATE memories SET category = 'preference' WHERE id = ?;" :
                mode == 4 ? "UPDATE memories SET subject_kind = 1, subject_id = 'another-owner' WHERE id = ?;" :
                "DELETE FROM memories WHERE id = ?;";
            sqlite3_stmt* statement = nullptr;
            Check(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK, "Could not prepare replacement mutation.");
            sqlite3_bind_text(statement, 1, newId.c_str(), -1, SQLITE_TRANSIENT);
            Check(sqlite3_step(statement) == SQLITE_DONE, "Could not mutate replacement.");
            sqlite3_finalize(statement);
        }
        sqlite3_close(db);
        const auto before = store.Load();
        MemoryRevisionReceipt receipt;
        Check(!store.SaveOwnerRevision(request, receipt, error) && receipt.requestId.empty() &&
            store.RevisionHistory().empty() && store.Load().size() == before.size(), "Stale replacement inserted an unreviewed third fact.");
    }
}

void TestReplacementCannotJoinAnotherReceiptChain()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    bool added; std::string first, second, error;
    const auto original = Decision("The owner tracks Amber at eight.");
    Check(store.Save(original, added, &first), "Separate chain seed failed.");
    const auto another = Decision("The owner tracks Amber at seven.");
    Check(store.Save(another, added, &second), "Separate chain seed failed.");
    MemoryRevisionReceipt receipt, rejected;
    auto request = Request(first, original.summary);
    Check(store.SaveOwnerRevision(request, receipt, error), error);
    request = Request(second, another.summary);
    request.ownerRequestId = "separate-chain-attempt";
    request.expectedReplacementId = receipt.revisedId;
    Check(!store.SaveOwnerRevision(request, rejected, error) && store.Load().size() == 3 && store.RevisionHistory().size() == 1,
        "Reviewed replacement joined competing chains or changed content.");
}
void TestScopeLeaseFailureAndCommitLifetime()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    bool added; std::string originalId, error;
    const auto original = Decision("The owner tracks Amber at eight.");
    Check(store.Save(original, added, &originalId), "Scope lease seed failed.");
    auto request = Request(originalId, original.summary);
    MemoryRevisionReceipt receipt;
    Check(!store.SaveOwnerRevision(request, receipt, error, {}, [] { return std::unique_ptr<MemoryRevisionScopeLease>{}; }),
        "A denied scope lease persisted a correction.");
    Check(!store.SaveOwnerRevision(request, receipt, error, {}, []() -> std::unique_ptr<MemoryRevisionScopeLease>
        { throw std::runtime_error("Scope fixture failure"); }), "A throwing scope lease escaped rejection.");
    struct Lease final : MemoryRevisionScopeLease
    {
        bool& held;
        explicit Lease(bool& value) : held(value) { held = true; }
        ~Lease() override { held = false; }
    };
    bool held = false;
    int checks = 0;
    Check(!store.SaveOwnerRevision(request, receipt, error, [&] { return held && ++checks == 1; },
        [&] { return std::make_unique<Lease>(held); }) && !held && store.Load().size() == 1 && store.RevisionHistory().empty(),
        "Retired commit gate retained content or leaked its scope lease.");
    checks = 0;
    Check(store.SaveOwnerRevision(request, receipt, error, [&] { ++checks; return held; },
        [&] { return std::make_unique<Lease>(held); }) && checks >= 2 && !held && store.RevisionHistory().size() == 1,
        "Scope lease was not retained through commit or released before return.");
}

void TestScopeLeaseAcquiredAfterConcurrentStoreAdmission()
{
    using namespace std::chrono_literals;
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    bool added; std::string originalId;
    const auto original = Decision("The owner tracks Amber at eight.");
    Check(store.Save(original, added, &originalId), "Concurrent scope fixture seed failed.");
    std::mutex audience;
    std::unique_lock ownerScope(audience);
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::atomic<int> automaticChecks = 0, leaseCalls = 0;
    auto automatic = std::async(std::launch::async, [&]
    {
        bool wasAdded;
        return store.Save(Decision("The owner grows orchids."), wasAdded, nullptr, [&]
        {
            if (++automaticChecks == 1) entered.set_value();
            const std::lock_guard lock(audience);
            return true;
        });
    });
    const bool reached = enteredFuture.wait_for(1s) == std::future_status::ready;
    struct ScopeLease final : MemoryRevisionScopeLease
    {
        std::unique_lock<std::mutex> lock;
        explicit ScopeLease(std::mutex& mutex) : lock(mutex) {}
    };
    auto correction = std::async(std::launch::async, [&]
    {
        MemoryRevisionReceipt receipt; std::string error;
        return store.SaveOwnerRevision(Request(originalId, original.summary), receipt, error, {}, [&]
        {
            ++leaseCalls;
            return std::make_unique<ScopeLease>(audience);
        });
    });
    const bool waitingForStore = correction.wait_for(100ms) == std::future_status::timeout && leaseCalls == 0;
    ownerScope.unlock();
    const bool automaticSaved = automatic.get(), corrected = correction.get();
    Check(reached && waitingForStore && automaticSaved && corrected && leaseCalls == 1 && store.RevisionHistory().size() == 1,
        "Owner correction acquired audience scope before concurrent store admission released the database.");
}
}

void TestRecallPresentationIsChronological()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = (fixture.root / "memory.db").string();
    longTermMemory store(path);
    bool added = false;
    const auto older = Decision("The owner planned the Amber expedition in spring.");
    const auto newer = Decision("The owner planned the Amber expedition in autumn.");
    Check(store.Save(older, added) && store.Save(newer, added), "Chronology fixture save failed.");
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Could not open chronology fixture.");
    const int result = sqlite3_exec(database,
        "UPDATE memories SET created_at = CASE WHEN summary LIKE '%spring.' THEN '1000000000' ELSE '1700000000' END;",
        nullptr, nullptr, nullptr);
    sqlite3_close(database);
    Check(result == SQLITE_OK, "Could not date chronology fixture.");
    const auto ranked = store.Search("Amber expedition", 2);
    Check(ranked.size() == 2 && ranked.front().summary == newer.summary,
        "Chronology fixture must select newer first by ranking.");
    const auto block = store.BuildPromptBlock("Amber expedition", 2, {}, "", 1800000000);
    Check(block.find(older.summary) < block.find(newer.summary),
        "Recalled facts must be presented oldest first after relevance selection.");
    Check(store.BuildPromptBlock("Amber expedition", 1).find(older.summary) == std::string::npos,
        "Chronological presentation must not change which facts fit the retrieval budget.");
}

void RunMemoryRevisionTests()
{
    TestRecallPresentationIsChronological();
    TestAppendOnlyRevisionRestartAndReversion();
    TestExactTargetsAdmissionAndStaleChains();
    TestReceiptFailureRollsBackCorrection();
    TestConcurrentExplicitRequestsRetainOneCurrentChain();
    TestLegacyDigestAndExactSavedReplacement();
    TestStaleReplacementRollsBackWithoutThirdFact();
    TestReplacementCannotJoinAnotherReceiptChain();
    TestScopeLeaseFailureAndCommitLifetime();
    TestScopeLeaseAcquiredAfterConcurrentStoreAdmission();
    std::cout << "Owner memory revision checks passed: immutable originals, exact receipt chains, rejection, restart and rollback.\n";
}
