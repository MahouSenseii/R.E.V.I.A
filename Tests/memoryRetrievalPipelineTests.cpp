#include "testSupport.h"
#include "Core/profile.h"
#include "LLM/promptBuilder.h"
#include "Memory/memoryReranker.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace
{
using revia::tests::Check;
using namespace revia::memory;

std::vector<nlohmann::json> Requests(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::vector<nlohmann::json> records;
    for (std::string line; std::getline(input, line);)
        records.push_back(nlohmann::json::parse(line));
    return records;
}

std::vector<std::string> Ids(const std::vector<memoryEntry>& rows)
{
    std::vector<std::string> ids;
    for (const auto& row : rows)
        ids.push_back(row.id);
    return ids;
}
}

int main(int argc, char** argv)
{
    try
    {
        Check(argc == 3, "Expected Python executable and reranker fixture path.");
        revia::tests::ScopedTestDirectory fixture;
        const auto databasePath = (fixture.root / "retrieval.db").string();
        const auto auditPath = fixture.root / "reranker-requests.jsonl";
        MemoryScope scope;
        scope.companionId = "retrieval-fixture";
        scope.participantId = "alice";
        scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
        scope.audience = {revia::identity::AudienceKind::Private, "fixture-private", 1, {"alice"}};
        {
            longTermMemory previousSession(databasePath);
            for (int index = 0; index < 75; ++index)
            {
                memoryDecision decision;
                decision.bSuccess = decision.bShouldRemember = true;
                decision.category = "project";
                decision.summary = (index < 35 ? "Alice project item " : "Bob SECRET project item ") + std::to_string(index) + ".";
                decision.subject = {MemorySubjectKind::Participant, index < 35 ? "alice" : "bob"};
                bool added = false;
                Check(previousSession.Save(decision, added) && added, "Could not seed scoped retrieval fixture.");
            }
        }
        sqlite3* database = nullptr;
        Check(sqlite3_open(databasePath.c_str(), &database) == SQLITE_OK, "Could not timestamp retrieval fixture.");
        const auto dated = sqlite3_exec(database, "UPDATE memories SET created_at = CAST(1700000000 + rowid AS TEXT);", nullptr, nullptr, nullptr);
        sqlite3_close(database);
        Check(dated == SQLITE_OK, "Could not assign ordered fixture dates.");
        // Recall must survive retiring the writer and opening the next session's store.
        longTermMemory store(databasePath);

        RerankerSettings settings;
        settings.enabled = true;
        settings.pythonExecutable = std::filesystem::absolute(argv[1]);
        settings.workerScript = std::filesystem::absolute(argv[2]);
        settings.modelDirectory = fixture.root;
        settings.startupTimeoutMs = 5000;
        settings.timeoutMs = 500;
        auto reranker = std::make_shared<MemoryReranker>();
        Check(reranker->Start(settings), "Could not start retrieval fixture reranker: " + reranker->LastError());
        const auto fused = store.Search("project", 30, {}, "", 0, &scope);
        Check(fused.size() == 30, "Fixture did not produce the full reranker shortlist.");
        const auto selected = store.Search("project", 6, {}, "", 0, &scope, reranker.get());
        Check(selected.size() == 6 && selected.front().id == fused.back().id,
              "Production Search truncated to six before reranking its top30.");
        for (std::size_t index = 0; index < selected.size(); ++index)
            Check(selected[index].id == fused[fused.size() - 1 - index].id, "Search did not apply the validated reranker order.");
        const auto received = Requests(auditPath);
        Check(received.size() == 1 && received.front()["documents"].size() == 30, "Search did not send one bounded top30 batch.");
        for (const auto& document : received.front()["documents"])
            Check(document.get<std::string>().find("SECRET") == std::string::npos, "Restricted subject reached the reranker.");

        auto expectedFallback = store.Search("project incomplete", 30, {}, "", 0, &scope);
        expectedFallback.resize(6);
        Check(Ids(store.Search("project incomplete", 6, {}, "", 0, &scope, reranker.get())) == Ids(expectedFallback),
              "Incomplete reranker output changed the expanded RRF fallback.");

        const auto prompt = store.BuildPromptBlock("project", 6, {}, "", 1800000000, &scope, reranker.get());
        auto chronological = selected;
        std::sort(chronological.begin(), chronological.end(), [](const auto& left, const auto& right) {
            return std::stoll(left.createdAt) < std::stoll(right.createdAt);
        });
        std::size_t position = 0;
        for (const auto& row : chronological)
        {
            const auto found = prompt.find(row.summary, position);
            Check(found != std::string::npos, "Reranked memory presentation was not oldest-first.");
            position = found + row.summary.size();
        }

        promptBuilder builder(databasePath);
        builder.SetMemoryReranker(reranker);
        aiProfile profile;
        profile.systemPrompt = "You are Revia.";
        profile.bMemoryEnabled = true;
        conversationMessage message{"user", "What project details have I told you?"};
        message.participantId = scope.participantId;
        message.memoryScope = scope;
        const std::vector<conversationMessage> context{message};
        const auto beforeDenied = Requests(auditPath).size();
        const auto denied = builder.BuildMessages(profile, context, {}, "", nullptr, "", nullptr,
                                                  revia::llm::PrivateMemoryAccess::Denied).dump();
        Check(Requests(auditPath).size() == beforeDenied && denied.find("Alice project item") == std::string::npos,
              "Private-memory denial sent a reranker request or exposed stored facts.");
        const auto admitted = builder.BuildMessages(profile, context).dump();
        Check(Requests(auditPath).size() == beforeDenied + 1 && admitted.find("Alice project item") != std::string::npos,
              "Private prompt did not reach production reranking and memory construction.");
        auto shared = scope;
        shared.audience.kind = revia::identity::AudienceKind::Shared;
        const auto beforeShared = Requests(auditPath).size();
        Check(store.BuildPromptBlock("project", 6, {}, "", 0, &shared, reranker.get()).empty() &&
              Requests(auditPath).size() == beforeShared, "Shared scope reached retrieval reranking.");
        std::stop_source cancelled;
        cancelled.request_stop();
        store.Search("project", 6, {}, "", 0, &scope, reranker.get(), cancelled.get_token());
        Check(Requests(auditPath).size() == beforeShared, "Cancelled retrieval called the reranker.");

        auto timeoutFallback = store.Search("project delay", 30, {}, "", 0, &scope);
        timeoutFallback.resize(6);
        Check(Ids(store.Search("project delay", 6, {}, "", 0, &scope, reranker.get())) == Ids(timeoutFallback) &&
              !reranker->IsAvailable(), "Timed-out production retrieval lost fallback or retained inference.");
        Check(Ids(store.Search("project", 6, {}, "", 0, &scope, reranker.get())) ==
              Ids(store.Search("project", 6, {}, "", 0, &scope)), "Unavailable worker changed ordinary RRF retrieval.");
        reranker->Stop();
        std::cout << "Memory shortlist, scope, prompt denial, chronology and timeout fallback pipeline checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
