#include "testSupport.h"
#include "Audit/contentDigest.h"
#include "Memory/longTermMemory.h"
#include "Memory/memoryReconciliation.h"

#include <iostream>
#include <atomic>
#include <barrier>
#include <thread>
#include <sqlite3.h>

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
}

void RunMemoryRevisionTests()
{
    TestAppendOnlyRevisionRestartAndReversion();
    TestExactTargetsAdmissionAndStaleChains();
    TestReceiptFailureRollsBackCorrection();
    TestConcurrentExplicitRequestsRetainOneCurrentChain();
    std::cout << "Owner memory revision checks passed: immutable originals, exact receipt chains, rejection, restart and rollback.\n";
}
