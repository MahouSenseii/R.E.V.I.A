#include "testSupport.h"
#include "Memory/longTermMemory.h"

#include <cstring>
#include <iostream>
#include <sqlite3.h>

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
}

void RunMemoryDedupTests()
{
    TestMeaningChangesRemainDistinct();
    TestDuplicateKeepsStoredTextAndVector();
    TestLegacyNormalizationDoesNotDiscardNewMeaning();
    std::cout << "Conservative memory deduplication tests passed.\n";
}