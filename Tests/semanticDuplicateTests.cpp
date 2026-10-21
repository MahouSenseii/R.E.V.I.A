#include "testSupport.h"
#include "Memory/longTermMemory.h"
#include <sqlite3.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <limits>
#include "Audit/contentDigest.h"

namespace
{
using revia::tests::Check;
using namespace revia::memory;
MemoryScope Scope()
{
    MemoryScope scope;
    scope.participantId = "fixture-owner";
    scope.companionId = "fixture-companion";
    scope.audience = {revia::identity::AudienceKind::Private, "fixture-private", 1, {"fixture-owner"}};
    scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    return scope;
}
SemanticDuplicateSettings Settings()
{
    SemanticDuplicateSettings result;
    result.enabled = true;
    result.modelIdentity = "fixture-nli@pinned-revision";
    return result;
}
memoryDecision Fact(const std::string& summary)
{
    memoryDecision value;
    value.bSuccess = value.bShouldRemember = true;
    value.category = "preference";
    value.summary = summary;
    value.subject = ParticipantSubject(Scope());
    value.embeddingModel = "fixture-embedding";
    value.embedding = {1.0F, 0.0F};
    return value;
}
struct Scorer : SemanticDuplicateScorer
{
    int calls = 0;
    NliPairResult result{true, false, Settings().modelIdentity, .995, .995, .002, .002};
    std::function<void()> during;
    NliPairResult Compare(const std::string&, const std::string&, std::stop_token) override
    {
        ++calls;
        if (during) during();
        return result;
    }
};
struct Fixture
{
    revia::tests::ScopedTestDirectory directory;
    std::string path = (directory.root / "memory.db").string();
    longTermMemory store{path};
    std::string older, newer;
    Fixture()
    {
        bool added;
        Check(store.Save(Fact("The user prefers concise answers."), added, &older) && added, "Older fixture failed.");
        Check(store.Save(Fact("The user likes short replies."), added, &newer) && added, "Newer fixture failed.");
    }
    SemanticDuplicateCheckResult Run(Scorer& scorer, std::function<bool()> admission = {}, std::stop_token token = {})
    {
        return CheckSemanticDuplicates(store, newer, "fixture-embedding", Scope(), Settings(), &scorer, token, admission);
    }
};
void Sql(const std::string& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Fixture database open failed.");
    const int status = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(database);
    Check(status == SQLITE_OK, "Fixture SQL failed.");
}
void TestMarksPreserveFactsAndRanking()
{
    Fixture f;
    const auto before = f.store.Search("answers replies", 6, {1,0}, "fixture-embedding");
    Scorer scorer;
    Check(f.Run(scorer).marked == 1 && scorer.calls == 1, "Strong safe pair was not marked.");
    const auto marks = f.store.LoadSemanticDuplicateMarks(Scope());
    Check(marks.size() == 1 && marks[0].memoryId == f.newer && marks[0].matchedMemoryId == f.older &&
        marks[0].memoryDigest == revia::audit::ContentDigest("The user likes short replies."), "Mark lost exact provenance.");
    Check(f.Run(scorer).marked == 0 && scorer.calls == 1, "Existing mark repeated model work.");
    const auto after = f.store.Search("answers replies", 6, {1,0}, "fixture-embedding");
    Check(before.size() == 2 && after.size() == 2 && before[0].id == after[0].id && before[1].id == after[1].id &&
        f.store.Load().size() == 2 && f.store.RevisionHistory().empty(), "Mark changed facts, ranking or correction authority.");
    longTermMemory restart(f.path);
    Check(restart.LoadSemanticDuplicateMarks(Scope()).size() == 1, "Advisory mark did not survive restart.");
    auto other = Scope(); other.participantId = "different-owner";
    Check(restart.LoadSemanticDuplicateMarks(other).empty(), "Mark crossed subject scope.");
    other = Scope(); other.audience.kind = revia::identity::AudienceKind::Public;
    Check(restart.LoadSemanticDuplicateMarks(other).empty(), "Mark crossed private-audience gate.");
    Sql(f.path, "PRAGMA foreign_keys=ON; DELETE FROM memories WHERE id='" + f.newer + "';");
    Check(restart.LoadSemanticDuplicateMarks(Scope()).empty() && restart.Load().size() == 1,
        "Advisory metadata prevented explicit fixture deletion or deleted the other fact.");
}
void TestFailClosedInferenceAndAdmission()
{
    Fixture f;
    Scorer scorer;
    auto disabled = Settings(); disabled.enabled = false;
    Check(CheckSemanticDuplicates(f.store, f.newer, "fixture-embedding", Scope(), disabled, &scorer).considered == 0,
        "Disabled feature reached inference.");
    Check(CheckSemanticDuplicates(f.store, f.newer, "fixture-embedding", Scope(), Settings(), nullptr).considered == 0,
        "Unconfigured feature reached inference.");
    Check(CheckSemanticDuplicates(f.store, f.newer, "wrong-vector-model", Scope(), Settings(), &scorer).considered == 0,
        "Wrong embedding model reached inference.");
    auto other = Scope(); other.participantId = "different-owner";
    Check(CheckSemanticDuplicates(f.store, f.newer, "fixture-embedding", other, Settings(), &scorer).considered == 0,
        "Different subject reached inference.");
    Check(f.Run(scorer, [] { return false; }).considered == 0 && scorer.calls == 0, "Denied admission reached inference.");
    std::stop_source stop; stop.request_stop();
    Check(f.Run(scorer, {}, stop.get_token()).considered == 0, "Cancelled request reached inference.");
    const auto good = scorer.result;
    for (int failure = 0; failure < 7; ++failure)
    {
        scorer.result = good;
        if (failure == 0) scorer.result.modelIdentity = "wrong-model";
        if (failure == 1) scorer.result.truncated = true;
        if (failure == 2) scorer.result.reverseEntailment = .97;
        if (failure == 3) scorer.result.reverseContradiction = .02;
        if (failure == 4) scorer.result.forwardEntailment = std::numeric_limits<double>::quiet_NaN();
        if (failure == 5) scorer.result.forwardContradiction = -0.1;
        if (failure == 6) scorer.result.succeeded = false;
        Check(f.Run(scorer).marked == 0, "Invalid or weak NLI verdict acquired a mark.");
    }
    scorer.result = good;
    bool admitted = true;
    scorer.during = [&] { admitted = false; };
    Check(f.Run(scorer, [&] { return admitted; }).marked == 0, "Retired context acquired a mark.");
    std::stop_source interrupted;
    scorer.during = [&] { interrupted.request_stop(); };
    Check(f.Run(scorer, {}, interrupted.get_token()).marked == 0, "In-flight cancellation acquired a mark.");
    scorer.during = [&] { Sql(f.path, "UPDATE memories SET active=0 WHERE id='" + f.older + "';"); };
    Check(f.Run(scorer).marked == 0 && f.store.LoadSemanticDuplicateMarks(Scope()).empty(), "Stale inactive record acquired a mark.");
}
void TestCorrectionWinsOverAdvisoryMark()
{
    Fixture f;
    Scorer scorer;
    Check(f.Run(scorer).marked == 1, "Could not seed correction precedence mark.");
    MemoryRevisionRequest request;
    request.ownerRequestId = "fixture-correction";
    request.originalId = f.newer;
    request.expectedSubject = ParticipantSubject(Scope());
    request.expectedSummaryDigest = revia::audit::ContentDigest("The user likes short replies.");
    request.origin = {"fixture-companion", "fixture-session", 1, {}, {}, 1};
    request.audienceRevision = 1;
    request.corrected = Fact("The user prefers detailed answers.");
    request.corrected.embedding.clear(); request.corrected.embeddingModel.clear();
    request.reason = "Owner requested this correction.";
    request.evidence = "Exact selected record and owner supplied replacement.";
    MemoryRevisionReceipt receipt; std::string error;
    Check(f.store.SaveOwnerRevision(request, receipt, error), error);
    Check(f.store.LoadSemanticDuplicateMarks(Scope()).empty() && f.Run(scorer).considered == 0 &&
        f.store.Load().size() == 3 && f.store.RevisionHistory().size() == 1,
        "Advisory mark competed with authoritative correction receipts.");
}
void TestLegacyMigrationAndBoundedShortlist()
{
    revia::tests::ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    Sql(path, "CREATE TABLE memories(id TEXT PRIMARY KEY,category TEXT,summary TEXT,normalized_summary TEXT UNIQUE,source TEXT,created_at TEXT,active INTEGER DEFAULT 1);"
        "INSERT INTO memories VALUES('legacy','fact','Legacy fact.','legacyfact','fixture','1',1);");
    longTermMemory store(path);
    Check(store.Load().size() == 1 && store.LoadSemanticDuplicateMarks(Scope()).empty(), "Legacy migration lost facts.");
    bool added; std::string id;
    for (const auto& text : {"The user prefers concise answers.", "The user wants brief responses.", "The user appreciates short explanations.",
        "The user favors concise replies.", "The user requests brief answers.", "The user enjoys short responses."})
        Check(store.Save(Fact(text), added, &id) && added, "Shortlist seed failed.");
    const auto candidates = store.SemanticDuplicateCandidates(id, "fixture-embedding", Scope(), Settings());
    Check(candidates.size() == 4, "Semantic duplicate shortlist exceeded or missed its fixed bound.");
    auto invalid = Settings(); invalid.maximumCandidates = 5;
    Check(store.SemanticDuplicateCandidates(id, "fixture-embedding", Scope(), invalid).empty(), "Unsafe shortlist size was accepted.");
    invalid = Settings(); invalid.minimumEntailment = .90;
    Check(!ValidSemanticDuplicateSettings(invalid), "Unsafe entailment threshold was accepted.");
}
void TestExpandedCorpusContract()
{
    const auto path = std::filesystem::path(__FILE__).parent_path() / "Data" / "semanticDuplicatePairs.json";
    std::ifstream input(path);
    Check(input.good(), "Expanded duplicate corpus missing.");
    nlohmann::json corpus; input >> corpus;
    std::set<std::string> ids, legacyIds;
    std::size_t heldout = 0, blocked = 0;
    for (const auto& entry : corpus.at("cases"))
    {
        Check(ids.insert(entry.at("id").get<std::string>()).second, "Duplicate corpus case ID.");
        const auto split = entry.at("split").get<std::string>();
        const auto label = entry.at("label").get<std::string>();
        Check(split == "development" || split == "heldout", "Corpus lacks frozen development/heldout partition.");
        Check(label == "equivalent" || label == "distinct" || label == "uncertain", "Unknown corpus equivalence label.");
        heldout += split == "heldout";
        if (entry.contains("legacy")) legacyIds.insert(entry.at("legacy").at("id").get<std::string>());
        if (entry.value("precheckMustReject", false))
        {
            Check(!MayCompareSemanticDuplicates(entry.at("left"), entry.at("right")), "Safety precheck missed case " + entry.at("id").get<std::string>());
            ++blocked;
        }
    }
    Check(ids.size() >= 64 && heldout >= 24 && legacyIds.size() == 24 && blocked >= 20,
        "Expanded corpus lost legacy traceability, heldout coverage or adversarial safety checks.");
}
void TestAdvisorySchemaIsAdditive()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = (fixture.root / "memory.db").string();
    longTermMemory store(path);
    memoryDecision decision;
    decision.bSuccess = decision.bShouldRemember = true;
    decision.summary = "The user prefers concise answers.";
    bool added = false;
    Check(store.Save(decision, added) && added, "Could not seed additive duplicate fixture.");
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Could not inspect duplicate schema.");
    sqlite3_stmt* query = nullptr;
    const bool prepared = sqlite3_prepare_v2(database,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='memory_duplicate_marks';", -1, &query, nullptr) == SQLITE_OK;
    const bool present = prepared && sqlite3_step(query) == SQLITE_ROW;
    sqlite3_finalize(query);
    sqlite3_close(database);
    Check(present, "The additive advisory duplicate-mark table is missing.");
    Check(store.Load().size() == 1 && store.Load().front().summary == decision.summary,
        "Adding duplicate bookkeeping altered a durable fact.");
}
}

void RunSemanticDuplicateTests()
{
    TestAdvisorySchemaIsAdditive();
    TestMarksPreserveFactsAndRanking();
    TestFailClosedInferenceAndAdmission();
    TestCorrectionWinsOverAdvisoryMark();
    TestLegacyMigrationAndBoundedShortlist();
    TestExpandedCorpusContract();
    std::cout << "Semantic duplicate mark checks passed.\n";
}
