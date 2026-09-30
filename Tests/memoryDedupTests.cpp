#include "Memory/memoryTypes.h"
#include "testSupport.h"
#include "Memory/longTermMemory.h"

#include <cstring>
#include <atomic>
#include <barrier>
#include <functional>
#include <iostream>
#include <sqlite3.h>
#include <thread>
#include <unordered_set>

namespace
{
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

memoryDecision Decision(const std::string& summary)
{
    memoryDecision decision;
    decision.bSuccess = decision.bShouldRemember = true;
    decision.category = "fact";
    decision.summary = summary;
    return decision;
}

// A one-shot SQLite hook schedules another real operation at the exact boundary
// under test. PROFILE runs after an INSERT completes; STMT runs before it starts.
// No assertions escape through SQLite's C callback, and nested SQL cannot reenter
// the action. Stores opened in this scope must be destroyed before the hook.
struct InsertBoundaryHook
{
    inline static InsertBoundaryHook* current = nullptr;
    sqlite3* connection = nullptr;
    unsigned int boundary = SQLITE_TRACE_PROFILE;
    std::function<void(sqlite3*)> action;
    bool armed = false, fired = false, callbackFailed = false;

    static int Trace(unsigned int event, void* context, void* statement, void*)
    {
        auto& hook = *static_cast<InsertBoundaryHook*>(context);
        const char* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if (!hook.armed || event != hook.boundary || !sql ||
            std::strncmp(sql, "INSERT OR IGNORE INTO memories ", 31) != 0)
            return 0;
        hook.armed = false;
        hook.fired = true;
        try { hook.action(hook.connection); }
        catch (...) { hook.callbackFailed = true; }
        return 0;
    }

    static int Open(sqlite3* database, char**, const sqlite3_api_routines*)
    {
        if (current && !current->connection)
        {
            current->connection = database;
            return sqlite3_trace_v2(database, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE,
                Trace, current);
        }
        return SQLITE_OK;
    }

    InsertBoundaryHook()
    {
        current = this;
        if (sqlite3_auto_extension(reinterpret_cast<void(*)()>(Open)) != SQLITE_OK)
        {
            current = nullptr;
            Check(false, "Could not instrument the memory fixture connection.");
        }
    }
    ~InsertBoundaryHook()
    {
        sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(Open));
        current = nullptr;
    }
    void CheckFired() const
    {
        Check(fired && !callbackFailed, "The deterministic INSERT boundary was not exercised.");
    }
};

void ExecuteFixture(const std::string& path, const char* sql)
{
    sqlite3* database = nullptr;
    const int opened = sqlite3_open(path.c_str(), &database);
    const int result = opened == SQLITE_OK
        ? sqlite3_exec(database, sql, nullptr, nullptr, nullptr) : opened;
    sqlite3_close(database);
    Check(result == SQLITE_OK, "Could not prepare the memory collision fixture.");
}

void TestInsertedRowSurvivesAnotherWriteCount()
{
    ScopedTestDirectory directory;
    InsertBoundaryHook hook;
    longTermMemory store((directory.root / "memory.db").string());
    store.HasMemories();
    int interleavedWrite = SQLITE_ERROR;
    hook.action = [&](sqlite3* database)
    {
        interleavedWrite = sqlite3_exec(database,
            "UPDATE memory_metadata SET value = value WHERE key = 'absent-fixture-key';",
            nullptr, nullptr, nullptr);
    };
    hook.armed = true;
    bool added = false;
    std::string id;
    const bool saved = store.Save(Decision("The user likes coffee."), added, &id);
    hook.CheckFired();
    Check(interleavedWrite == SQLITE_OK, "The interleaved zero-row write failed.");
    Check(saved && added && !id.empty(),
        "A real INSERT was reported as a failure after another write changed the connection count.");
    const auto entries = store.Load();
    Check(entries.size() == 1 && entries.front().id == id,
        "A successful memory save returned an id that was not persisted.");
}

void TestIgnoredRowSurvivesAnotherWriteCount()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    InsertBoundaryHook hook;
    longTermMemory store(path);
    bool added = false;
    Check(store.Save(Decision("The user likes coffee."), added) && added,
        "Could not seed the ignored-insert fixture.");
    ExecuteFixture(path, "UPDATE memories SET active = 0;");
    int interleavedWrite = SQLITE_ERROR;
    hook.action = [&](sqlite3* database)
    {
        interleavedWrite = sqlite3_exec(database,
            "UPDATE memory_metadata SET value = value WHERE key = 'legacy_jsonl_imported';",
            nullptr, nullptr, nullptr);
    };
    hook.armed = true;
    added = true;
    std::string id = "unwritten";
    const bool saved = store.Save(Decision("The user likes coffee."), added, &id);
    hook.CheckFired();
    Check(interleavedWrite == SQLITE_OK, "The interleaved one-row write failed.");
    Check(!saved && !added && id.empty() && store.Load().empty(),
        "An ignored INSERT reported success using another write's connection count.");
}

void TestRacingDuplicateReturnsRetainedRow(const bool formatted)
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    InsertBoundaryHook hook;
    longTermMemory first(path), second(path);
    first.HasMemories();
    second.HasMemories();
    auto original = Decision("The user likes coffee.");
    auto candidate = original;
    candidate.embeddingModel = "race-fixture";
    candidate.embedding = {0.0F, 1.0F};
    if (formatted)
    {
        original.embeddingModel = "race-fixture";
        original.embedding = {1.0F, 0.0F};
        candidate.summary = "  THE user\tlikes  coffee.\n";
    }
    bool winnerSaved = false, winnerAdded = false;
    std::string winnerId;
    hook.boundary = SQLITE_TRACE_STMT;
    hook.action = [&](sqlite3*) { winnerSaved = second.Save(original, winnerAdded, &winnerId); };
    hook.armed = true;
    bool added = true;
    std::string id = "unwritten";
    const bool saved = first.Save(candidate, added, &id);
    hook.CheckFired();
    Check(winnerSaved && winnerAdded && !winnerId.empty(), "The competing fixture save failed.");
    Check(saved && !added && id == winnerId,
        "A save racing an active exact duplicate did not return the retained row's real id.");
    const auto entries = first.Load();
    Check(entries.size() == 1 && entries.front().summary == original.summary,
        "Duplicate recovery altered the retained summary.");
    const auto& expectedVector = formatted ? original.embedding : candidate.embedding;
    sqlite3_stmt* query = nullptr;
    const int prepared = sqlite3_prepare_v2(hook.connection,
        "SELECT vector FROM memory_embeddings WHERE model = 'race-fixture';", -1, &query, nullptr);
    const bool matches = prepared == SQLITE_OK && sqlite3_step(query) == SQLITE_ROW &&
        sqlite3_column_bytes(query, 0) == static_cast<int>(expectedVector.size() * sizeof(float)) &&
        std::memcmp(sqlite3_column_blob(query, 0), expectedVector.data(),
            expectedVector.size() * sizeof(float)) == 0;
    sqlite3_finalize(query);
    Check(matches, formatted ? "Racing formatted duplicate replaced the retained text's vector."
        : "Racing exact-text duplicate could not fill the missing vector.");
    if (formatted)
    {
        candidate.embeddingModel = "race-new-model";
        Check(first.Save(candidate, added) && !added &&
            first.LoadMissingEmbeddings("race-new-model").size() == 1,
            "A formatted duplicate filled an embedding for different retained text.");
    }
}

void TestUnrelatedCollisionDoesNotBecomeDuplicateSuccess(const bool idCollision)
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    longTermMemory store(path);
    bool added = false;
    Check(store.Save(Decision("The user likes tea."), added) && added,
        "Could not seed the unrelated collision fixture.");
    ExecuteFixture(path, idCollision
        ? "CREATE TRIGGER fixture_id_collision BEFORE INSERT ON memories "
          "WHEN new.summary = 'The user likes coffee.' BEGIN "
          "UPDATE memories SET id = new.id WHERE summary = 'The user likes tea.'; END;"
        : "UPDATE memories SET normalized_summary = 'v2:the user likes coffee.';");
    added = true;
    std::string id = "unwritten";
    Check(!store.Save(Decision("The user likes coffee."), added, &id) && !added && id.empty(),
        "An unrelated id/key collision was treated as an active exact duplicate.");
    const auto entries = store.Load();
    Check(entries.size() == 1 && entries.front().summary == "The user likes tea.",
        "Collision recovery discarded or replaced an unrelated memory.");
}

void TestConcurrentDistinctClaimsRemainDurable()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    longTermMemory shared(path), other(path);
    shared.HasMemories();
    other.HasMemories();
    constexpr int workerCount = 4, batches = 12;
    const char* claims[] = {
        "The user likes coffee.", "The user does not like coffee.",
        "The limit is 10.", "The limit is 100.",
        "Alice trusts Bob.", "Bob trusts Alice.",
        "Coffee is allowed before noon.", "Coffee is allowed after noon."
    };
    std::barrier start(workerCount);
    std::atomic<int> failed{0};
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < workerCount; ++worker)
    {
        workers.emplace_back([&, worker]
        {
            start.arrive_and_wait();
            try
            {
                auto& store = worker < 2 ? shared : other;
                for (int batch = 0; batch < batches; ++batch)
                {
                    for (int offset = 0; offset < 2; ++offset)
                    {
                        const std::string summary = std::string(claims[worker * 2 + offset]) +
                            " Fixture batch " + std::to_string(batch) + ".";
                        bool added = false;
                        std::string id;
                        if (!store.Save(Decision(summary), added, &id) || !added || id.empty())
                            ++failed;
                    }
                }
            }
            catch (...) { ++failed; }
        });
    }
    workers.clear(); // Join every writer before checking externally visible results.
    Check(failed == 0, "Concurrent distinct memory saves reported a failure or an unwritten id.");
    const auto entries = shared.Load();
    std::unordered_set<std::string> summaries, ids;
    for (const auto& entry : entries)
    {
        summaries.insert(entry.summary);
        ids.insert(entry.id);
    }
    constexpr auto expected = static_cast<std::size_t>(workerCount * batches * 2);
    Check(entries.size() == expected && summaries.size() == expected && ids.size() == expected,
        "Concurrent saves lost distinct opposite, numeric, conditional, or subject claims.");
}

void TestLegacyImportCompletesInsertedAndIgnoredRows()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    {
        std::ofstream legacy(directory.root / "memory.jsonl");
        legacy << R"({"id":"legacy-first","summary":"The user likes coffee.","createdAt":"100"})" << '\n'
               << R"({"id":"legacy-repeat","summary":"The user likes coffee.","createdAt":"101"})" << '\n'
               << R"({"id":"legacy-next","summary":"The user likes tea.","createdAt":"102"})" << '\n';
    }
    {
        longTermMemory store(path);
        const auto entries = store.Load();
        Check(entries.size() == 2 && entries.front().id == "legacy-first" &&
            entries.back().id == "legacy-next", "Legacy import did not complete past an ignored duplicate.");
    }
    longTermMemory reopened(path);
    Check(reopened.Load().size() == 2, "Legacy import did not commit its completed statements.");
}

void TestMeaningChangesRemainDistinct()
{
    const std::pair<const char*, const char*> cases[] = {
        {"The user likes coffee.", "The user no longer likes coffee."},
        {"The user likes coffee.", "The user does not like coffee."},
        {"The user drinks coffee.", "The user never drinks coffee."},
        {"The user drinks coffee.", "The user stopped drinking coffee."},
        {"The user drinks coffee.", "The user drinks tea instead of coffee."},
        {"The user drinks coffee.", "The user changed from coffee to tea."},
        {"The user uses C++.", "The user uses C#."},
        {"The user uses .NET.", "The user uses NET."},
        {"The user uses version 1.2.3.", "The user uses version 12.3."},
        {"The offset is -10.", "The offset is 10."},
        {"Alice trusts Bob.", "Bob trusts Alice."},
        {"The identifier is ab.", "The identifier is a b."}
    };
    for (const auto& [original, correction] : cases)
    {
        ScopedTestDirectory directory;
        longTermMemory store((directory.root / "memory.db").string());
        bool added = false;
        std::string firstId, secondId;
        Check(store.Save(Decision(original), added, &firstId) && added, "Could not save original memory.");
        Check(store.Save(Decision(correction), added, &secondId) && added,
            "Meaning-bearing change was discarded: " + std::string(correction));
        Check(firstId != secondId && store.Load().size() == 2, "Distinct propositions share a memory row.");
    }
}

void TestDuplicateKeepsStoredTextAndVector()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    longTermMemory store(path);
    auto original = Decision("The user likes coffee.");
    original.embeddingModel = "fixture";
    original.embedding = {1.0F, 0.0F};
    bool added = false;
    std::string id, duplicateId;
    Check(store.Save(original, added, &id) && added, "Could not save vector fixture.");
    Check(store.Save(original, added, &duplicateId) && !added && id == duplicateId,
        "Exact duplicate was not deduplicated.");
    auto formatted = original;
    formatted.summary = "  THE user\tlikes  coffee.\n";
    formatted.embedding = {0.0F, 1.0F};
    Check(store.Save(formatted, added, &duplicateId) && !added && id == duplicateId,
        "Harmless case/whitespace differences did not deduplicate.");
    const auto entries = store.Load();
    Check(entries.size() == 1 && entries.front().summary == original.summary,
        "A duplicate changed the retained text.");

    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Could not inspect persisted vector.");
    sqlite3_stmt* query = nullptr;
    const int prepared = sqlite3_prepare_v2(database,
        "SELECT vector FROM memory_embeddings WHERE model = 'fixture';", -1, &query, nullptr);
    const bool matches = prepared == SQLITE_OK && sqlite3_step(query) == SQLITE_ROW &&
        sqlite3_column_bytes(query, 0) == static_cast<int>(original.embedding.size() * sizeof(float)) &&
        std::memcmp(sqlite3_column_blob(query, 0), original.embedding.data(),
            original.embedding.size() * sizeof(float)) == 0;
    sqlite3_finalize(query);
    sqlite3_close(database);
    Check(matches, "Duplicate paired retained text with a different input's embedding.");

    formatted.embeddingModel = "new-model";
    Check(store.Save(formatted, added) && !added, "Duplicate with a new embedding model failed.");
    Check(store.LoadMissingEmbeddings("new-model").size() == 1,
        "Different duplicate text supplied an embedding for the retained summary.");
    original.embeddingModel = "new-model";
    Check(store.Save(original, added) && !added && store.LoadMissingEmbeddings("new-model").empty(),
        "An exact-text duplicate could not fill its missing embedding.");
}

void TestLegacyNormalizationDoesNotDiscardNewMeaning()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    {
        longTermMemory store(path);
        bool added = false;
        Check(store.Save(Decision("ab"), added) && added, "Could not seed legacy fixture.");
    }
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Could not open legacy fixture.");
    const int result = sqlite3_exec(database, "UPDATE memories SET normalized_summary = 'ab';",
        nullptr, nullptr, nullptr);
    sqlite3_close(database);
    Check(result == SQLITE_OK, "Could not reproduce legacy normalized key.");
    longTermMemory reopened(path);
    bool added = false;
    Check(reopened.Save(Decision("a b"), added) && added && reopened.Load().size() == 2,
        "Legacy normalized key discarded a distinct new summary.");
    Check(reopened.Save(Decision("AB"), added) && !added && reopened.Load().size() == 2,
        "Legacy row stopped deduplicating after reopening.");
}
// Deduplication looks only at active rows; the unique key spans every row. Nothing
// deactivates a memory today, so this reproduces by hand what the first forget feature
// would produce, and pins the outcome as a loud failure rather than a success carrying
// an id that was never written.
void TestDiscardedInsertDoesNotReportSuccess()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    {
        longTermMemory store(path);
        bool added = false;
        Check(store.Save(Decision("The user likes coffee."), added) && added,
            "Could not seed the collision fixture.");
    }
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Could not open fixture.");
    const int result = sqlite3_exec(database, "UPDATE memories SET active = 0;",
        nullptr, nullptr, nullptr);
    sqlite3_close(database);
    Check(result == SQLITE_OK, "Could not deactivate the fixture row.");

    longTermMemory reopened(path);
    bool added = true;
    std::string id = "unwritten";
    Check(!reopened.Save(Decision("The user likes coffee."), added, &id),
        "An insert discarded by the unique key reported success.");
    Check(!added && id.empty(),
        "A discarded insert returned an id for a row that does not exist.");
}
}

void RunMemoryDedupTests()
{
    TestMeaningChangesRemainDistinct();
    TestDuplicateKeepsStoredTextAndVector();
    TestLegacyNormalizationDoesNotDiscardNewMeaning();
    TestDiscardedInsertDoesNotReportSuccess();
    TestInsertedRowSurvivesAnotherWriteCount();
    TestIgnoredRowSurvivesAnotherWriteCount();
    TestRacingDuplicateReturnsRetainedRow(false);
    TestRacingDuplicateReturnsRetainedRow(true);
    TestUnrelatedCollisionDoesNotBecomeDuplicateSuccess(false);
    TestUnrelatedCollisionDoesNotBecomeDuplicateSuccess(true);
    TestConcurrentDistinctClaimsRemainDurable();
    TestLegacyImportCompletesInsertedAndIgnoredRows();
    std::cout << "Conservative memory deduplication tests passed.\n";
}
