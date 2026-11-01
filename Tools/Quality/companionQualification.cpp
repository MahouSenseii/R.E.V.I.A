#include "../../Tests/Fixture/qualificationAccess.h"
#include "Core/configManager.h"
#include "Memory/longTermMemory.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
#include <httplib.h>
#include <sqlite3.h>
#include <windows.h>

namespace
{
using namespace revia::runtime;
using json = nlohmann::json;
using namespace std::chrono_literals;

void Check(bool value, const std::string& message)
{
    if (!value) throw std::runtime_error(message);
}

json Read(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    return json::parse(stream);
}

void Receipt(const std::filesystem::path& path, const json& data)
{
    Check(!std::filesystem::exists(path), "Phase receipt must be fresh.");
    std::ofstream output(path, std::ios::binary);
    output << data.dump(2) << '\n';
    output.close();
    Check(!output.fail(), "Phase receipt close failed.");
}

class ProcessGuard
{
public:
    ProcessGuard()
    {
        job = CreateJobObjectW(nullptr, nullptr);
        Check(job != nullptr, "Cannot create fixture process guard.");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        Check(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
            AssignProcessToJobObject(job, GetCurrentProcess()), "Cannot forbid fixture descendants.");
    }
    ~ProcessGuard() { CloseHandle(job); }
private:
    HANDLE job = nullptr;
};

json Relationship(const ReviaSession& session)
{
    const auto relationship = session.CurrentRelationship();
    return {{"entity_id", relationship.entityId}, {"display_name", relationship.displayName},
        {"interactions", relationship.interactionCount}, {"first_seen", relationship.firstSeenAt}};
}

json PersistedSpeaker(const ReviaSession& session)
{
    for (const auto& relationship : session.Relationships())
        if (relationship.displayName == "QualificationOwner")
            return {{"entity_id", relationship.entityId}, {"display_name", relationship.displayName},
                {"interactions", relationship.interactionCount}, {"first_seen", relationship.firstSeenAt}};
    return json::object();
}

json Revision(const revia::memory::MemoryRevisionReceipt& revision)
{
    return {{"request_id", revision.requestId}, {"digest", revision.digest}, {"original_id", revision.originalId},
        {"revised_id", revision.revisedId}, {"subject_id", revision.subject.entityId}, {"created_at", revision.createdAt}};
}

json MemorySnapshot(const longTermMemory& store)
{
    auto data = json::array();
    for (const auto& row : store.Load())
        data.push_back({{"id", row.id}, {"summary", row.summary}, {"subject", row.subject.entityId},
            {"current_id", row.currentRevisionId}, {"receipt", row.revisionReceiptId}});
    return data;
}

class Provider
{
public:
    Provider(int port, std::string key) : port(port), key(std::move(key)) {}

    void Stage(const std::string& stage) const
    {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(2);
        client.set_read_timeout(2);
        const auto response = client.Post("/fixture/stage", {{"Authorization", "Bearer " + key}},
            json{{"stage", stage}}.dump(), "application/json");
        Check(static_cast<bool>(response), "Owned provider transport unavailable: " + httplib::to_string(response.error()) +
            "; winsock=" + std::to_string(WSAGetLastError()));
        Check(response->status == 200, "Owned provider stage returned HTTP " + std::to_string(response->status));
        Check(json::parse(response->body).value("id", "") == "qual1-synthetic-provider-v1", "Owned provider identity mismatch.");
    }

    std::string Turn(ReviaSession& session, const std::string& stage, const std::string& input) const
    {
        Stage(stage);
        const auto answer = session.Submit(input);
        Check(answer.succeeded && answer.fromAssistant, "Production Submit failed at " + stage + ": " + answer.reason);
        return answer.text;
    }
private:
    int port;
    std::string key;
};

template<class Predicate>
bool WaitFor(ReviaSession& session, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        session.PollBackgroundEvents();
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

void WriteCorrect(ReviaSession& session, const Provider& provider, const std::string& fault,
    const std::filesystem::path& database, const std::filesystem::path& receipt, json& result)
{
    longTermMemory store(database.string());
    if (fault != "empty-state")
    {
        provider.Turn(session, "write/initial", "I use Cedar as Project Juniper's label.");
        Check(WaitFor(session, [&]
            {
                const auto rows = store.Load();
                return std::any_of(rows.begin(), rows.end(), [](const auto& row)
                    { return row.summary == "The user uses Cedar as Project Juniper's label."; });
            }), "Initial extracted memory was not saved.");
        result["checks"]["initial_automatic_save"] = "passed";
        provider.Turn(session, "write/correction", "Actually, I now use Maple as Project Juniper's label.");
        Check(WaitFor(session, [&] { return !session.PendingMemoryCorrections().empty(); }), "Automatic proposal did not arrive.");
        const auto proposals = session.PendingMemoryCorrections();
        const auto exact = std::find_if(proposals.begin(), proposals.end(), [](const auto& proposal)
            { return proposal.original.summary == "The user uses Cedar as Project Juniper's label." &&
                proposal.corrected.summary == "The user uses Maple as Project Juniper's label."; });
        Check(exact != proposals.end(), "Exact admitted correction proposal missing.");
        result["checks"]["automatic_proposal"] = "passed";
        std::string error;
        revia::memory::MemoryRevisionReceipt revision;
        if (fault == "interrupt-before-receipt")
        {
            result["status"] = "interrupted";
            result["checks"]["durable_acceptance"] = "not_run";
            Receipt(receipt, result);
            std::_Exit(73);
        }
        if (fault == "rejected-correction")
        {
            Check(session.RejectMemoryCorrectionProposal(exact->id, error), error);
            Check(store.RevisionHistory().empty(), "Rejected proposal wrote a revision.");
            result["checks"]["rejection_without_revision"] = "passed";
        }
        else if (fault == "failed-save")
        {
            sqlite3* lock = nullptr;
            Check(sqlite3_open(database.string().c_str(), &lock) == SQLITE_OK, "Cannot open owned lock fixture.");
            const int began = sqlite3_exec(lock, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
            Check(began == SQLITE_OK, "Cannot acquire owned save blocker.");
            const bool accepted = session.AcceptMemoryCorrectionProposal(exact->id, revision, error);
            sqlite3_exec(lock, "ROLLBACK", nullptr, nullptr, nullptr);
            sqlite3_close(lock);
            Check(!accepted && store.RevisionHistory().empty(), "Save blocker produced durable acceptance.");
            result["checks"]["durable_save_refused"] = "passed";
        }
        else
        {
            Check(session.AcceptMemoryCorrectionProposal(exact->id, revision, error), error);
            const auto history = store.RevisionHistory();
            Check(history.size() == 1 && history.front().requestId == revision.requestId && !revision.digest.empty(),
                "Durable receipt missing or unrelated to accepted correction.");
            result["durable_receipt"] = Revision(revision);
            result["checks"]["durable_acceptance"] = "passed";
        }
    }
    result["revision_count"] = store.RevisionHistory().size();
    result["memory_after_write"] = MemorySnapshot(store);
}

void Recall(ReviaSession& session, const Provider& provider, const std::filesystem::path& database, json& result)
{
    longTermMemory store(database.string());
    result["revision_count"] = store.RevisionHistory().size();
    result["answer"] = provider.Turn(session, "recall/private", "What is Project Juniper's current label?");
    const auto before = MemorySnapshot(store);
    const auto revisions = store.RevisionHistory().size();
    std::string error;
    Check(session.SetAudience({revia::identity::AudienceKind::Public, "qualification-public", 0, {}}, error), error);
    result["public_answer"] = provider.Turn(session, "recall/public", "What is Project Juniper's current label?");
    session.PollBackgroundEvents();
    result["checks"]["public_private_rows_unchanged"] = before == MemorySnapshot(store) ? "passed" : "failed";
    result["checks"]["public_private_revisions_unchanged"] = revisions == store.RevisionHistory().size() ? "passed" : "failed";
    result["private_rows_before_public"] = before;
    result["private_rows_after_public"] = MemorySnapshot(store);
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "qualification-private", 0, {}}, error), error);
    provider.Turn(session, "recall/reintroduction", "My name is QualificationOwner.");
    result["private_control_answer"] = provider.Turn(session, "recall/private-control", "What is Project Juniper's current label?");
}
}

int main(int argc, char** argv)
{
    json result = {{"schema_version", 1}, {"scope", "fixture"}, {"real_model", "not_run"},
        {"status", "failed"}, {"checks", json::object()}, {"durable_receipt", nullptr}, {"pid", GetCurrentProcessId()}};
    std::filesystem::path receipt;
    try
    {
        std::map<std::string, std::string> args;
        Check(argc == 15, "Expected exactly seven named worker arguments.");
        for (int i = 1; i < argc; i += 2) Check(args.emplace(argv[i], argv[i + 1]).second, "Duplicate worker argument.");
        const auto phase = args.at("--phase");
        const auto fault = args.at("--fault");
        Check(fault == "none" || fault == "empty-state" || fault == "rejected-correction" || fault == "failed-save" ||
            fault == "retrieval-disabled" || fault == "interrupt-before-receipt", "Unknown fixture fault.");
        Check(phase == "preflight" || phase == "transport" || phase == "write-correct" || phase == "recall", "Unknown phase.");
        const std::filesystem::path root(args.at("--runtime-root"));
        receipt = args.at("--receipt");
        Check(root.is_absolute() && receipt.is_absolute() && !std::filesystem::exists(receipt), "Invalid captured paths.");
        const auto runId = args.at("--run-id");
        Check(Read(root / "qualification-owner.json").value("run_id", "") == runId, "Runtime ownership mismatch.");
        result.update({{"phase", phase}, {"fault", fault}, {"run_id", runId}, {"runtime_root", root.string()}});
        const int port = std::stoi(args.at("--provider-port"));
        const char* value = std::getenv(args.at("--api-key-env").c_str());
        Check(value && std::string(value).size() >= 16, "Named credential unavailable.");
        const std::string key(value);
        ProcessGuard guard;
        result["checks"]["no_child_processes_allowed"] = "passed";
        std::filesystem::current_path(root);
        const CompanionPaths paths(root, {"qualification", "Qualification fixture", "fixture", false});
        const auto database = paths.Resolve("Memory/revia_memory.db");
        if (phase == "recall")
        {
            Check(std::filesystem::is_regular_file(database), "Recall cannot bootstrap missing memory.");
            Check(std::filesystem::is_regular_file(paths.Resolve("RuntimeData/Identity/identity.json")), "Recall requires persisted identity.");
        }
        appSettings settings;
        configManager config((root / "Config/settings.json").string(), paths.Resolve("Config/Profiles").string());
        Check(config.LoadSettings(settings), "Captured settings did not load.");
        result["configuration_before_start"] = ReviaSessionTestAccess::DisabledHosts(settings);
        for (const auto& setting : result["configuration_before_start"].items())
            Check(setting.value() == false, "Optional host enabled: " + setting.key());
        const auto profile = Read(paths.Resolve("Config/Profiles/fixture.json"));
        Check(profile.value("memoryEnabled", false) && !profile.value("shouldSpeak", true), "Memory-enabled silent profile required.");
        Check(!std::filesystem::exists(paths.Resolve("RuntimeData/VirtualMachine/settings.json")), "VM configuration is forbidden.");
        Check(settings.llm.backend == "LLamaCpp" && settings.llm.host == "127.0.0.1" && settings.llm.port == port && settings.llm.apiKey.empty(),
            "Local transport configuration is not isolated and credential-free.");
        Check(settings.conversation.restoreTurns == 0 && settings.conversation.delegation == "off", "Conversation replay/delegation enabled.");
        Check(std::getenv("REVIA_WEB_ENABLED") == nullptr, "Ambient public listener opt-in forbidden.");
        result["checks"]["optional_hosts_disabled_before_start"] = "passed";
        result["checks"]["memory_enabled_no_archive_tail"] = "passed";
        result["credential_binding"] = "env key copied only to router transport; session.settings remains credential-free of env key";
        if (phase == "preflight")
        {
            result["status"] = "passed";
            result["production_start"] = "not_run";
            Receipt(receipt, result);
            return 0;
        }
        Provider provider(port, key);
        if (phase == "transport")
        {
            result["production_start"] = "not_run";
            provider.Stage("transport-probe");
            result["status"] = "passed";
            Receipt(receipt, result);
            return 0;
        }
        ReviaSession session(paths);
        Check(session.Start(), "Production Start failed.");
        result["configuration_effective"] = ReviaSessionTestAccess::DisabledHosts(session);
        Check(result["configuration_effective"] == result["configuration_before_start"], "Effective optional host settings changed.");
        Check(ReviaSessionTestAccess::QualificationMemoryEnabled(session), "Effective profile disabled memory.");
        ReviaSessionTestAccess::QualificationTransport(session, key, fault != "retrieval-disabled");
        result["companion_id"] = session.Stamp().companionId;
        result["default_current_relationship"] = Relationship(session);
        result["relationship_loaded"] = PersistedSpeaker(session);
        if (phase == "recall") Check(!result["relationship_loaded"].empty(), "Persisted named relationship missing before introduction.");
        result["working_context_before_introduction"] = ReviaSessionTestAccess::QualificationContextSize(session);
        Check(result["working_context_before_introduction"] == 0, "Restored history is forbidden in restart recall.");
        provider.Turn(session, phase + "/introduction", "My name is QualificationOwner.");
        result["relationship"] = Relationship(session);
        Check(result["relationship"]["display_name"] == "QualificationOwner", "Named participant was not admitted.");
        if (phase == "write-correct") WriteCorrect(session, provider, fault, database, receipt, result);
        else Recall(session, provider, database, result);
        result["relationship"] = Relationship(session);
        session.Stop();
        result["checks"]["production_stop"] = "passed";
        result["status"] = "passed";
        Receipt(receipt, result);
        return 0;
    }
    catch (const std::exception& error)
    {
        result["error"] = error.what();
        if (!receipt.empty() && !std::filesystem::exists(receipt))
        {
            try { Receipt(receipt, result); } catch (...) {}
        }
        std::cerr << "Fixture worker failed; inspect its fresh receipt.\n";
        return 1;
    }
}
