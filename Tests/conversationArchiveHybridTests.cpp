#include "testSupport.h"
#include "Memory/conversationArchive.h"
#include "Memory/conversationRecall.h"

#include <algorithm>
#include <limits>
#include <sqlite3.h>

namespace
{
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using namespace revia::memory;

MemoryScope PrivateScope(const std::string& participant = "alice")
{
    MemoryScope scope;
    scope.companionId = "fixture-companion";
    scope.participantId = participant;
    scope.audience = {revia::identity::AudienceKind::Private, "private-room", 3, {participant}};
    scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    return scope;
}

void Sql(const std::string& path, const char* sql)
{
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Archive fixture database did not open.");
    sqlite3_busy_timeout(database, 2000);
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
    const std::string error = sqlite3_errmsg(database);
    sqlite3_close(database);
    Check(result == SQLITE_OK, "Archive fixture SQL failed: " + error);
}

int VectorCount(const std::string& path)
{
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Archive fixture count did not open.");
    sqlite3_stmt* statement = nullptr;
    const bool prepared =
        sqlite3_prepare_v2(database, "SELECT COUNT(*) FROM conversation_embeddings;", -1, &statement, nullptr) == SQLITE_OK;
    const int result = prepared && sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    if (statement)
        sqlite3_finalize(statement);
    sqlite3_close(database);
    Check(result >= 0, "Archive embedding count failed.");
    return result;
}

ArchivedTurn Add(ConversationArchive& archive, const std::string& session, const std::string& content, const MemoryScope& scope,
    const std::string& role = "user")
{
    std::string error;
    Check(archive.BeginSession(session, error) && archive.Record(session, role, content, error, scope),
        "Could not seed archived turn: " + error);
    return archive.LoadSession(session, 500).back();
}

RecallRequest Topic(const std::string& term)
{
    RecallRequest request;
    request.kind = RecallKind::Topic;
    request.terms = {term};
    return request;
}

void TestArchiveHybridRecallKeepsGates()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "archive.db").string();
    ConversationArchive archive(path);
    const auto alice = PrivateScope(), bob = PrivateScope("bob");
    auto semantic = Add(archive, "private-a", "I spent the summer beside the ocean.", alice);
    auto lexical = Add(archive, "private-a", "My vacation planning happened at the desk.", alice);
    Check(archive.SaveEmbedding(semantic, "fixture-model", {1.0F, 0.0F}), "Semantic fixture vector failed.");
    for (int index = 0; index < 25; ++index)
    {
        const auto denied = Add(archive, "private-b", "A vacation beside a lake " + std::to_string(index), bob);
        Check(archive.SaveEmbedding(denied, "fixture-model", {1.0F, 0.0F}), "Denied fixture vector failed.");
    }
    auto request = Topic("vacation");
    const auto lexicalOnly = archive.SearchHybrid(request, 2, alice);
    Check(lexicalOnly.size() == 1 && lexicalOnly.front().id == lexical.id, "Lexical archive fallback changed.");
    const auto hybrid = archive.SearchHybrid(request, 2, alice, {1.0F, 0.0F}, "fixture-model");
    Check(hybrid.size() == 2 && hybrid.front().id == semantic.id, "Archive hybrid did not recover the paraphrased turn.");
    Check(std::all_of(hybrid.begin(), hybrid.end(), [&](const auto& turn) { return turn.scope.participantId == alice.participantId; }),
        "Semantic archive recall crossed participant scope.");
    Check(archive.SearchHybrid({}, 2, alice, {1.0F, 0.0F}, "fixture-model").empty(),
        "An unrequested archive lookup admitted semantic results.");
    auto shared = alice;
    shared.audience.kind = revia::identity::AudienceKind::Shared;
    Check(archive.SearchHybrid(request, 2, shared, {1.0F, 0.0F}, "fixture-model").empty(),
        "Shared audience received archive semantic recall.");
    auto otherCompanion = alice;
    otherCompanion.companionId = "other-companion";
    Check(archive.SearchHybrid(request, 2, otherCompanion, {1.0F, 0.0F}, "fixture-model").empty(),
        "Another companion received archive semantic recall.");
    for (const auto& vector : std::vector<std::vector<float>>{{1.0F}, {0.0F, 0.0F}, {std::numeric_limits<float>::quiet_NaN(), 0.0F}})
    {
        const auto fallback = archive.SearchHybrid(request, 2, alice, vector, "fixture-model");
        Check(
            fallback.size() == 1 && fallback.front().id == lexical.id, "Invalid or wrong-dimension query did not retain lexical fallback.");
    }
    Check(archive.SearchHybrid(request, 2, alice, {1.0F, 0.0F}, "wrong-model").size() == 1,
        "Archive compared vectors from different models.");
    Check(archive.Search("vacation").size() == 12, "Explicit history phrase search behavior changed.");
}

void TestArchiveTimeAndVoiceGates()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "archive.db").string();
    ConversationArchive archive(path);
    auto scope = PrivateScope();
    auto old = Add(archive, "older", "The blue pottery cup.", scope);
    auto recent = Add(archive, "recent", "A porcelain vessel.", scope);
    auto future = Add(archive, "future", "A porcelain plate.", scope);
    auto voiceScope = scope;
    voiceScope.participantSource = revia::identity::SpeakerSource::ConsentedVoice;
    voiceScope.consentRevision = 9;
    Add(archive, "voice", "Another porcelain bowl.", voiceScope);
    Sql(path, "UPDATE conversation_turns SET created_at=CASE session_id "
              "WHEN 'older' THEN '100' WHEN 'recent' THEN '200' WHEN 'voice' THEN '220' ELSE '400' END;");
    for (const auto& session : {"older", "recent", "future", "voice"})
    {
        const auto turn = archive.LoadSession(session).front();
        Check(archive.SaveEmbedding(turn, "fixture-model", {1.0F, 0.0F}), "Temporal vector failed.");
    }
    auto request = Topic("pottery");
    request.kind = RecallKind::Window;
    request.window = {150, 300, "fixture-window"};
    const auto window = archive.SearchHybrid(request, 10, scope, {1.0F, 0.0F}, "fixture-model", 300);
    Check(window.size() == 1 && window.front().id == recent.id, "Hybrid window crossed time or voice-consent bounds.");
    scope.consentRevision = 9;
    const auto consented = archive.SearchHybrid(request, 10, scope, {1.0F, 0.0F}, "fixture-model", 300);
    Check(consented.size() == 2, "Current recognized-voice consent did not admit its own archive.");
    request.terms.clear();
    const auto chronological = archive.SearchHybrid(request, 10, scope, {1.0F, 0.0F}, "fixture-model", 300);
    Check(chronological.size() == 2 && chronological.front().createdAt == "200" && chronological.back().createdAt == "220",
        "An empty-topic window lost its chronological semantics.");
    request = Topic("pottery");
    request.kind = RecallKind::Earliest;
    const auto earliest = archive.SearchHybrid(request, 10, scope, {1.0F, 0.0F}, "fixture-model", 300);
    Check(earliest.size() == 1 && earliest.front().id == old.id, "Earliest-mention lookup inferred a first mention from a vector.");
    request.kind = RecallKind::Topic;
    const auto topic = archive.SearchHybrid(request, 10, scope, {1.0F, 0.0F}, "fixture-model", 300);
    Check(topic.size() == 3 && std::none_of(topic.begin(), topic.end(), [&](const auto& turn) { return turn.id == future.id; }),
        "Topic semantic recall admitted a future-dated turn.");
}

void TestArchiveEmbeddingPersistenceAndRetraction()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "archive.db").string();
    auto scope = PrivateScope();
    ConversationArchive archive(path);
    const auto first = Add(archive, "replace", "I chose a clay cup.", scope);
    const auto second = Add(archive, "other", "I chose a bronze cup.", scope);
    const auto page = archive.ScanMissingEmbeddings("fixture-model", 0, 1, &scope);
    Check(page.error.empty() && page.entries.size() == 1 && page.hasMore, "Archive backfill pagination did not report its tail.");
    const auto next = archive.ScanMissingEmbeddings("fixture-model", page.nextRowId, 1, &scope);
    Check(next.entries.size() == 1 && !next.hasMore && next.entries.front().id != page.entries.front().id,
        "Archive backfill cursor repeated or skipped its next row.");
    Check(!archive.SaveEmbedding(first, "", {1.0F, 0.0F}) && !archive.SaveEmbedding(first, "fixture-model", {0.0F, 0.0F}) &&
              !archive.SaveEmbedding(first, "fixture-model", {std::numeric_limits<float>::infinity(), 0.0F}),
        "Archive accepted a malformed vector or absent model.");
    int admissions = 0;
    Check(!archive.SaveEmbedding(first, "fixture-model", {1.0F, 0.0F}, [&]() { return ++admissions == 1; }) &&
              archive.NeedsEmbedding(first.id, "fixture-model"),
        "Expired archive write admission committed a vector.");
    Check(archive.SaveEmbedding(first, "fixture-model", {1.0F, 0.0F}) && !archive.NeedsEmbedding(first.id, "fixture-model") &&
              archive.NeedsEmbedding(first.id, "next-model"),
        "Archive vector model tracking failed.");
    Check(ConversationArchive(path).SearchHybrid(Topic("ceramics"), 2, scope, {1.0F, 0.0F}, "fixture-model").size() == 1,
        "A persisted archive vector did not survive reopening.");
    Check(
        archive.RemoveLastTurnIf("replace", "user", first.content) && VectorCount(path) == 0, "Retraction left its archive vector behind.");
    const auto replacement = Add(archive, "replace", "I chose a glass cup.", scope);
    Check(replacement.id == first.id && !archive.SaveEmbedding(first, "fixture-model", {1.0F, 0.0F}),
        "A delayed vector was attached to a regenerated turn with a reused ID.");
    Check(archive.SaveEmbedding(replacement, "fixture-model", {1.0F, 0.0F}) && archive.SaveEmbedding(second, "fixture-model", {1.0F, 0.0F}),
        "Replacement vectors failed.");
    Check(archive.ForgetSession("replace") == 1 && VectorCount(path) == 1, "Session forgetting left archive vectors behind.");
    Check(!archive.SaveEmbedding(replacement, "fixture-model", {1.0F, 0.0F}), "Forgotten turn was resurrected by vector backfill.");
    Check(archive.Forget() == 1 && VectorCount(path) == 0, "Full archive forgetting left vectors behind.");
    Check(archive.ScanMissingEmbeddings("fixture-model", 0).entries.empty(), "Forgotten turns reappeared in backfill.");
    Check(!ConversationArchive(directory.root.string()).ScanMissingEmbeddings("fixture-model", 0).error.empty(),
        "An unavailable archive was reported as an empty successful backfill.");
}

void TestLegacyArchiveMigrationAndRetention()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "legacy.db").string();
    Sql(path, "CREATE TABLE conversation_sessions(session_id TEXT PRIMARY KEY,started_at TEXT NOT NULL,ended_at TEXT);"
              "CREATE TABLE conversation_turns(id TEXT NOT NULL UNIQUE,session_id TEXT NOT NULL,turn_index INTEGER NOT NULL,"
              "role TEXT NOT NULL,content TEXT NOT NULL,created_at TEXT NOT NULL);"
              "INSERT INTO conversation_sessions VALUES('legacy','100','200');"
              "INSERT INTO conversation_turns VALUES('legacy:0','legacy',0,'user','The legacy pottery mug.','100');"
              "CREATE VIRTUAL TABLE conversation_search USING fts5(content,content='conversation_turns',content_rowid='rowid',"
              "tokenize='unicode61 remove_diacritics 2');"
              "INSERT INTO conversation_search(conversation_search) VALUES('rebuild');");
    const auto scope = PrivateScope();
    for (int pass = 0; pass < 2; ++pass)
    {
        ConversationArchive migrated(path);
        Check(migrated.LoadSession("legacy").size() == 1 && migrated.Search("pottery").size() == 1,
            "Archive embedding migration lost a legacy transcript or its lexical index.");
        Check(migrated.ScanMissingEmbeddings("fixture-model", 0, 25, &scope).entries.empty(),
            "Archive migration assigned the current participant to an unattributed legacy row.");
        const auto turn = migrated.LoadSession("legacy").front();
        Check(migrated.SaveEmbedding(turn, "fixture-model", {1.0F, 0.0F}), "Legacy vector migration write failed.");
        Check(migrated.SearchHybrid(Topic("pottery"), 10, scope, {1.0F, 0.0F}, "fixture-model").empty(),
            "An unattributed legacy vector entered private automatic recall.");
    }
    ConversationArchive bounded(path, {1, 500, 8000});
    std::string error;
    Check(bounded.BeginSession("newer", error) && !bounded.HasSession("legacy") && VectorCount(path) == 0,
        "Retention pruning left an expired conversation vector behind.");
}
}

void RunConversationArchiveHybridTests()
{
    TestArchiveHybridRecallKeepsGates();
    TestArchiveTimeAndVoiceGates();
    TestArchiveEmbeddingPersistenceAndRetraction();
    TestLegacyArchiveMigrationAndRetention();
    ScopedTestDirectory directory;
    const std::string path = (directory.root / "media.db").string();
    const auto scope = PrivateScope();
    const std::vector<conversationMediaReference> refs = {{"media-one", std::string(64, 'a'), "001.png", "image/png", 40, 30},
        {"media-two", std::string(64, 'b'), "002.png", "image/png", 50, 20}};
    std::string error;
    {
        ConversationArchive archive(path);
        Check(archive.BeginSession("images", error) && archive.Record("images", "user", "Compare these images.", error, scope, {}, refs),
            "Could not archive image associations: " + error);
        Check(archive.LoadSession("images").front().mediaReferences == refs, "Archived image associations lost their order or identity.");
    }
    ConversationArchive restored(path);
    Check(restored.LoadSessionTail("images", 10, scope).front().mediaReferences == refs,
        "Reopening an archive lost the user's image associations.");
    Check(restored.LoadSessionTail("images", 10, PrivateScope("bob")).empty(), "Another participant restored image associations.");
    Check(restored.Search("images").front().mediaReferences == refs, "An archive query omitted image associations.");
    Check(restored.Record("images", "assistant", "sentinel_private_image_analysis", error, scope) &&
              restored.InvalidateMediaAnalysis(refs.front().id),
        "Forgotten image analysis was not invalidated.");
    const auto cleared = restored.LoadSession("images");
    Check(cleared.front().mediaReferences == refs && cleared.back().content.find("sentinel_private_image_analysis") == std::string::npos &&
              restored.Search("sentinel_private_image_analysis").empty(),
        "Forgotten media left derived analysis in archive recall.");
    Check(restored.RemoveLastTurnIf("images", "assistant", cleared.back().content), "Could not retract the invalidated analysis notice.");
    Check(restored.RemoveLastTurnIf("images", "user", "Compare these images.") && restored.LoadSession("images").empty(),
        "Retracting a media turn retained its old association.");
    Check(restored.Record("images", "user", "A replacement without media.", error, scope) &&
              restored.LoadSession("images").front().mediaReferences.empty(),
        "A reused archive index resurrected retracted media.");
    auto invalid = refs;
    invalid.front().digest = "not-a-digest";
    Check(!restored.Record("images", "user", "Invalid metadata.", error, scope, {}, invalid),
        "Invalid media identity was archived as an authorized association.");
    auto gif = refs;
    gif.front().mimeType = "image/gif";
    gif.front().name = "001.gif";
    Check(restored.Record("images", "user", "Inspect this still frame.", error, scope, {}, gif) &&
              restored.LoadSession("images").back().mediaReferences == gif,
        "A supported native GIF still-frame association was lost from the archive.");
}
