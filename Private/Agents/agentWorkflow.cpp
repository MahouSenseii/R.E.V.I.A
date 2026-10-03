#include "Agents/agentWorkflow.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace revia::agents
{
namespace
{
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
constexpr std::size_t MaximumMaterial = 8192;
constexpr std::size_t MaximumCheckpoint = 524288;

bool Identifier(const std::string& value)
{
    return !value.empty() && value.size() <= 160 &&
           std::all_of(value.begin(), value.end(),
               [](const unsigned char ch)
               {
                   return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                          ch == '.';
               });
}
bool Digest(const std::string& value)
{
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](const char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}
bool ReportedModel(const std::string& value)
{
    return !value.empty() && value.size() <= 160 &&
           std::all_of(value.begin(), value.end(), [](const unsigned char ch) { return ch >= 32 && ch != 127; });
}
std::string InputFingerprint(const std::string& input, const std::string& evidence)
{
    return AgentWorkflow::ArtifactHash(std::to_string(input.size()) + ":" + input + std::to_string(evidence.size()) + ":" + evidence);
}
std::uint64_t Milliseconds(const Clock::time_point start)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}
bool StampValid(const runtime::RuntimeStamp& stamp, const std::string& workflow)
{
    return Identifier(stamp.companionId) && Identifier(stamp.sessionId) && stamp.generation > 0 && stamp.taskId == workflow;
}
Json StampJson(const runtime::RuntimeStamp& value)
{
    return {{"companion", value.companionId}, {"session", value.sessionId}, {"generation", value.generation}, {"task", value.taskId},
        {"attempt", value.attemptId}, {"policy", value.policyVersion}};
}
runtime::RuntimeStamp ReadStamp(const Json& value)
{
    return {value.at("companion"), value.at("session"), value.at("generation"), value.at("task"), value.at("attempt"), value.at("policy")};
}
Json ReferenceJson(const ArtifactReference& value)
{
    return {{"node", value.nodeId}, {"id", value.id}, {"version", value.version}, {"hash", value.hash}};
}
ArtifactReference ReadReference(const Json& value)
{
    return {value.at("node"), value.at("id"), value.at("version"), value.at("hash")};
}
bool AtomicSave(const std::filesystem::path& path, const std::string& data)
{
    if (path.empty() || data.size() > MaximumCheckpoint)
        return false;
    auto temporary = path;
    temporary += ".pending-" + std::to_string(Clock::now().time_since_epoch().count());
    std::error_code error;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    bool saved = false;
#ifdef _WIN32
    const HANDLE file = CreateFileW(temporary.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        saved = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size() &&
                FlushFileBuffers(file);
        saved = CloseHandle(file) && saved;
        if (saved)
            saved = MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
#else
    const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file >= 0)
    {
        const auto written = ::write(file, data.data(), data.size());
        saved = written == static_cast<ssize_t>(data.size()) && ::fsync(file) == 0;
        saved = ::close(file) == 0 && saved;
        if (saved)
            saved = ::rename(temporary.c_str(), path.c_str()) == 0;
    }
#endif
    if (!saved)
        std::filesystem::remove(temporary, error);
    return saved;
}
}

std::string ToString(const WorkflowRole value)
{
    switch (value)
    {
    case WorkflowRole::Parent:
        return "Parent";
    case WorkflowRole::Worker:
        return "Worker";
    case WorkflowRole::Reviewer:
        return "Reviewer";
    }
    return "Unknown";
}
std::string ToString(const WorkflowState value)
{
    switch (value)
    {
    case WorkflowState::Queued:
        return "Queued";
    case WorkflowState::Waiting:
        return "Waiting";
    case WorkflowState::Running:
        return "Running";
    case WorkflowState::Paused:
        return "Paused";
    case WorkflowState::Succeeded:
        return "Succeeded";
    case WorkflowState::Failed:
        return "Failed";
    case WorkflowState::Interrupted:
        return "Interrupted";
    case WorkflowState::Cancelled:
        return "Cancelled";
    case WorkflowState::Exhausted:
        return "Exhausted";
    case WorkflowState::AwaitingAcceptance:
        return "Awaiting acceptance";
    case WorkflowState::Accepted:
        return "Accepted";
    case WorkflowState::Rejected:
        return "Rejected";
    }
    return "Unknown";
}
std::string ToString(const ParentDecision value)
{
    switch (value)
    {
    case ParentDecision::Pending:
        return "Pending";
    case ParentDecision::Accept:
        return "Accept";
    case ParentDecision::Rework:
        return "Rework";
    case ParentDecision::Reject:
        return "Reject";
    case ParentDecision::NeedEvidence:
        return "Need evidence";
    }
    return "Unknown";
}

std::string AgentWorkflow::ArtifactHash(const std::string& content)
{
    if (content.size() > MaximumMaterial + 512)
        return {};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    const bool succeeded =
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(content.data())), static_cast<ULONG>(content.size()), 0) >= 0 &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!succeeded)
        return {};
    std::string result;
    for (const auto byte : digest)
    {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
#else
    return {};
#endif
}

bool AgentWorkflow::Validate(const WorkflowSpec& spec, std::string& error)
{
    const auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    if (!Identifier(spec.id) || spec.id.size() > 64 || spec.nodes.size() != 4)
        return fail("The workflow requires one parent, two workers and one reviewer.");
    const auto& budget = spec.budget;
    if (budget.maximumParallel == 0 || budget.maximumParallel > 2 || budget.maximumAttemptsPerNode == 0 ||
        budget.maximumAttemptsPerNode > 4 || budget.maximumRequests == 0 || budget.maximumRequests > 32 ||
        budget.maximumActiveMilliseconds == 0 || budget.maximumActiveMilliseconds > 3600000 || budget.maximumReportedTokens == 0 ||
        budget.maximumReportedTokens > 1000000)
        return fail("Workflow budgets must be positive and within hard bounds.");
    std::map<std::string, const WorkflowNode*> nodes;
    const WorkflowNode* parent = nullptr;
    const WorkflowNode* reviewer = nullptr;
    std::set<std::string> workers;
    for (const auto& node : spec.nodes)
    {
        if (!Identifier(node.id) || node.id.size() > 64 || !nodes.emplace(node.id, &node).second || node.label.empty() ||
            node.label.size() > 160 || node.objective.size() > MaximumMaterial || node.inputText.size() > MaximumMaterial ||
            node.evidenceKey.size() > 256 || !node.readOnly)
            return fail("Node identity/material must be bounded and providers must be read-only.");
        switch (node.role)
        {
        case WorkflowRole::Parent:
            if (parent || node.level != 3 || !node.parentId.empty())
                return fail("Parent hierarchy is invalid.");
            parent = &node;
            break;
        case WorkflowRole::Reviewer:
            if (reviewer || node.level != 1)
                return fail("Reviewer hierarchy is invalid.");
            reviewer = &node;
            break;
        case WorkflowRole::Worker:
            if (node.level != 1)
                return fail("Worker hierarchy is invalid.");
            workers.insert(node.id);
            break;
        default:
            return fail("Unknown node role.");
        }
    }
    if (!parent || !reviewer || workers.size() != 2)
        return fail("Workflow roles are incomplete.");
    for (const auto& node : spec.nodes)
    {
        if (node.role != WorkflowRole::Parent && node.parentId != parent->id)
            return fail("A node has an unknown parent.");
        std::set<std::string> unique;
        for (const auto& dependency : node.dependsOn)
            if (!nodes.contains(dependency) || !unique.insert(dependency).second)
                return fail("A dependency is missing or duplicated.");
    }
    std::set<std::string> visiting, visited;
    const std::function<bool(const std::string&)> visit = [&](const std::string& id)
    {
        if (visiting.contains(id))
            return false;
        if (visited.contains(id))
            return true;
        visiting.insert(id);
        for (const auto& dependency : nodes.at(id)->dependsOn)
            if (!visit(dependency))
                return false;
        visiting.erase(id);
        visited.insert(id);
        return true;
    };
    for (const auto& node : spec.nodes)
        if (!visit(node.id))
            return fail("The dependency graph contains a cycle.");
    if (std::set<std::string>(reviewer->dependsOn.begin(), reviewer->dependsOn.end()) != workers ||
        parent->dependsOn != std::vector<std::string>{reviewer->id})
        return fail("Both workers must feed the reviewer before separate parent acceptance.");
    for (const auto& id : workers)
        if (!nodes.at(id)->dependsOn.empty())
            return fail("The two bounded workers must be independent.");
    error.clear();
    return true;
}

struct AgentWorkflow::Impl
{
    struct Attempt
    {
        WorkflowAttempt view;
        std::string fingerprint;
        std::string privateDiagnostic;
        std::optional<WorkflowArtifact> artifact;
    };
    struct Node
    {
        WorkflowNode spec;
        WorkflowNodeSnapshot view;
        std::vector<Attempt> history;
        std::optional<WorkflowArtifact> artifact;
        Clock::time_point changed = Clock::now();
    };
    mutable std::mutex mutex;
    std::mutex control;
    std::condition_variable_any changed;
    WorkflowSpec spec;
    WorkflowSnapshot summary;
    std::vector<Node> nodes;
    Provider provider;
    Observer observer;
    std::stop_source cancellation;
    std::jthread runner;
    std::vector<std::jthread> jobs;
    std::vector<std::thread::id> jobIds;
    std::thread::id runnerId;
    std::thread::id drainerId;
    bool running = false;
    bool draining = false;
    bool parentEvaluation = false;
    std::uint64_t era = 0;
    std::size_t active = 0;
    std::deque<WorkflowSnapshot> notifications;

    Node& Find(const std::string& id)
    {
        return *std::find_if(nodes.begin(), nodes.end(), [&](const auto& node) { return node.spec.id == id; });
    }
    bool OwnedThread() const
    {
        const auto id = std::this_thread::get_id();
        return id == runnerId || (draining && id == drainerId) || std::find(jobIds.begin(), jobIds.end(), id) != jobIds.end();
    }
    void Transition(Node& node, const WorkflowState state)
    {
        const auto elapsed = Milliseconds(node.changed);
        if (node.view.state == WorkflowState::Running)
            node.view.activeMilliseconds += elapsed;
        else if (node.view.state == WorkflowState::Waiting || node.view.state == WorkflowState::Queued ||
                 node.view.state == WorkflowState::AwaitingAcceptance)
            node.view.waitingMilliseconds += elapsed;
        else if (node.view.state == WorkflowState::Paused || node.view.state == WorkflowState::Interrupted)
            node.view.pausedMilliseconds += elapsed;
        node.changed = Clock::now();
        node.view.state = state;
    }
    bool Ready(const Node& node)
    {
        return std::all_of(node.spec.dependsOn.begin(), node.spec.dependsOn.end(),
            [&](const auto& id)
            {
                const auto& source = Find(id);
                return source.view.state == WorkflowState::Succeeded && source.artifact.has_value();
            });
    }
    WorkflowSnapshot SnapshotLocked() const
    {
        auto value = summary;
        for (const auto& node : nodes)
        {
            auto view = node.view;
            view.workingOn = node.spec.label;
            view.currentAction = view.state == WorkflowState::Running ? "Executing the bounded provider." : ToString(view.state);
            view.waitingFor = view.state == WorkflowState::Waiting              ? "Verified prerequisite artifacts or execution capacity."
                              : view.state == WorkflowState::AwaitingAcceptance ? "Separate parent acceptance."
                                                                                : std::string{};
            view.nextHandoff = node.spec.role == WorkflowRole::Worker     ? "Reviewer"
                               : node.spec.role == WorkflowRole::Reviewer ? "Parent acceptance"
                                                                          : "Runtime";
            const auto elapsed = Milliseconds(node.changed);
            if (view.state == WorkflowState::Running)
                view.activeMilliseconds += elapsed;
            else if (view.state == WorkflowState::Waiting || view.state == WorkflowState::Queued ||
                     view.state == WorkflowState::AwaitingAcceptance)
                view.waitingMilliseconds += elapsed;
            else if (view.state == WorkflowState::Paused || view.state == WorkflowState::Interrupted)
                view.pausedMilliseconds += elapsed;
            for (const auto& attempt : node.history)
            {
                auto record = attempt.view;
                if (record.state == WorkflowState::Running)
                    record.activeMilliseconds = elapsed;
                view.attempts.push_back(std::move(record));
            }
            value.nodes.push_back(std::move(view));
        }
        return value;
    }
    void ObserveLocked()
    {
        ++summary.sequence;
        if (!observer)
            return;
        // Full snapshots contain cumulative attempt history; superseded snapshots may coalesce.
        if (notifications.size() == 32)
            notifications.pop_front();
        notifications.push_back(SnapshotLocked());
    }
    void Drain()
    {
        {
            std::lock_guard lock(mutex);
            if (draining)
                return;
            draining = true;
            drainerId = std::this_thread::get_id();
        }
        while (true)
        {
            WorkflowSnapshot value;
            Observer callback;
            {
                std::lock_guard lock(mutex);
                if (notifications.empty())
                {
                    draining = false;
                    drainerId = {};
                    return;
                }
                value = std::move(notifications.front());
                notifications.pop_front();
                callback = observer;
            }
            if (callback)
                try
                {
                    callback(value);
                }
                catch (...)
                {
                }
        }
    }
    std::uint64_t ActiveTime() const
    {
        std::uint64_t total = 0;
        for (const auto& node : nodes)
            total += node.view.activeMilliseconds + (node.view.state == WorkflowState::Running ? Milliseconds(node.changed) : 0);
        return total;
    }
    void Complete(const std::string& id, const std::uint64_t capturedEra, NodeResult result)
    {
        {
            std::lock_guard lock(mutex);
            auto& node = Find(id);
            auto& attempt = node.history.back();
            attempt.view.activeMilliseconds = Milliseconds(node.changed);
            attempt.view.executionSucceeded = result.succeeded;
            attempt.privateDiagnostic = result.diagnostic.substr(0, 1024);
            attempt.view.reportedTokens = result.reportedTokens;
            attempt.view.reportedModel = result.reportedModel && ReportedModel(*result.reportedModel) ? result.reportedModel : std::nullopt;
            if (result.reportedTokens && *result.reportedTokens <= 1000000000)
                summary.reportedTokens += *result.reportedTokens;
            else
            {
                ++summary.unreportedRequests;
                attempt.view.reportedTokens.reset();
            }
            WorkflowState state = WorkflowState::Failed;
            const bool current = capturedEra == era && !cancellation.stop_requested();
            const bool validArtifact = result.artifact.version > 0 && Identifier(result.artifact.id) && !result.artifact.content.empty() &&
                                       result.artifact.content.size() <= MaximumMaterial && Digest(result.artifact.hash) &&
                                       result.artifact.hash == AgentWorkflow::ArtifactHash(result.artifact.content);
            if (!current)
                state = WorkflowState::Cancelled;
            else if (result.succeeded && result.verified && validArtifact)
            {
                state = WorkflowState::Succeeded;
                attempt.view.verified = true;
                node.artifact = std::move(result.artifact);
                attempt.artifact = node.artifact;
                attempt.view.artifact = ArtifactReference{id, node.artifact->id, node.artifact->version, node.artifact->hash};
            }
            attempt.view.state = state;
            attempt.view.diagnostic = state == WorkflowState::Cancelled ? "The attempt was cancelled; its late result was not admitted."
                                      : state == WorkflowState::Failed
                                          ? (result.succeeded ? "Execution completed, but verified evidence was not established."
                                                              : "Provider execution did not complete.")
                                          : "Execution and artifact verification completed; parent acceptance remains separate.";
            Transition(node, state);
            --active;
            ObserveLocked();
        }
        changed.notify_all();
        Drain();
    }
    void Run(const std::uint64_t capturedEra)
    {
        while (true)
        {
            bool stopForBudget = false;
            {
                std::unique_lock lock(mutex);
                const bool timedOut = ActiveTime() >= spec.budget.maximumActiveMilliseconds;
                if (cancellation.stop_requested() || timedOut)
                {
                    stopForBudget = timedOut && !cancellation.stop_requested();
                    summary.state = timedOut ? WorkflowState::Exhausted : WorkflowState::Cancelled;
                    for (auto& node : nodes)
                        if (node.view.state == WorkflowState::Waiting || node.view.state == WorkflowState::Queued)
                            Transition(node, summary.state);
                }
                if (!cancellation.stop_requested() && !timedOut)
                {
                    for (auto& node : nodes)
                    {
                        if ((node.spec.role == WorkflowRole::Parent && !parentEvaluation) || node.view.state != WorkflowState::Waiting ||
                            !Ready(node) || active >= spec.budget.maximumParallel)
                            continue;
                        if (summary.requests >= spec.budget.maximumRequests ||
                            summary.reportedTokens >= spec.budget.maximumReportedTokens ||
                            node.history.size() >= spec.budget.maximumAttemptsPerNode)
                        {
                            Transition(node, WorkflowState::Exhausted);
                            summary.state = WorkflowState::Exhausted;
                            continue;
                        }
                        NodeRequest request;
                        request.node = node.spec;
                        request.attempt = static_cast<std::uint32_t>(node.history.size() + 1);
                        request.stamp = summary.stamp;
                        request.stamp.attemptId = spec.id + "." + node.spec.id + "." + std::to_string(++summary.requests);
                        Attempt attempt;
                        attempt.view.id = request.stamp.attemptId;
                        attempt.view.ordinal = request.attempt;
                        attempt.view.stamp = request.stamp;
                        attempt.view.state = WorkflowState::Running;
                        attempt.fingerprint = InputFingerprint(node.spec.inputText, node.spec.evidenceKey);
                        for (const auto& dependency : node.spec.dependsOn)
                        {
                            const auto& source = Find(dependency);
                            request.prerequisites.push_back(*source.artifact);
                            attempt.view.prerequisites.push_back(
                                {dependency, source.artifact->id, source.artifact->version, source.artifact->hash});
                        }
                        node.history.push_back(std::move(attempt));
                        Transition(node, WorkflowState::Running);
                        ++active;
                        summary.state = WorkflowState::Running;
                        const auto call = provider;
                        const auto token = cancellation.get_token();
                        jobs.emplace_back(
                            [this, request = std::move(request), call, token, capturedEra]()
                            {
                                NodeResult result;
                                try
                                {
                                    result = call(request, token);
                                }
                                catch (...)
                                {
                                }
                                Complete(request.node.id, capturedEra, std::move(result));
                            });
                        jobIds.push_back(jobs.back().get_id());
                        ObserveLocked();
                    }
                }
                if (active == 0)
                {
                    auto& parent =
                        *std::find_if(nodes.begin(), nodes.end(), [](const auto& node) { return node.spec.role == WorkflowRole::Parent; });
                    if (Ready(parent) && (!parentEvaluation || parent.view.state == WorkflowState::Succeeded) &&
                        !cancellation.stop_requested() && !timedOut)
                    {
                        if (!parentEvaluation)
                            Transition(parent, WorkflowState::AwaitingAcceptance);
                        summary.state = WorkflowState::AwaitingAcceptance;
                    }
                    else if (summary.state != WorkflowState::Cancelled && summary.state != WorkflowState::Exhausted)
                    {
                        summary.state = WorkflowState::Paused;
                        if (!parentEvaluation)
                            Transition(parent, WorkflowState::Paused);
                    }
                    ObserveLocked();
                    break;
                }
            }
            if (stopForBudget)
                cancellation.request_stop();
            Drain();
            std::unique_lock lock(mutex);
            changed.wait_for(lock, std::chrono::milliseconds(10));
        }
        for (auto& job : jobs)
            if (job.joinable())
                job.join();
        {
            std::lock_guard lock(mutex);
            running = false;
            jobIds.clear();
        }
        changed.notify_all();
        Drain();
    }
    Json EncodeLocked() const
    {
        const auto snapshot = SnapshotLocked();
        const auto& budget = spec.budget;
        Json value = {{"schema", 1}, {"id", spec.id}, {"stamp", StampJson(snapshot.stamp)}, {"state", snapshot.state},
            {"decision", snapshot.parentDecision}, {"sequence", snapshot.sequence}, {"requests", snapshot.requests},
            {"reportedTokens", snapshot.reportedTokens}, {"unreportedRequests", snapshot.unreportedRequests},
            {"parentEvaluation", parentEvaluation},
            {"budget",
                {{"parallel", budget.maximumParallel}, {"attempts", budget.maximumAttemptsPerNode}, {"requests", budget.maximumRequests},
                    {"activeMilliseconds", budget.maximumActiveMilliseconds}, {"reportedTokens", budget.maximumReportedTokens}}},
            {"nodes", Json::array()}};
        for (std::size_t index = 0; index < nodes.size(); ++index)
        {
            const auto& node = nodes[index];
            const auto& view = snapshot.nodes[index];
            Json row = {{"id", node.spec.id}, {"parent", node.spec.parentId}, {"role", node.spec.role}, {"level", node.spec.level},
                {"label", node.spec.label}, {"objective", node.spec.objective}, {"input", node.spec.inputText},
                {"evidence", node.spec.evidenceKey}, {"dependencies", node.spec.dependsOn}, {"readOnly", node.spec.readOnly},
                {"state", view.state}, {"activeMilliseconds", view.activeMilliseconds}, {"waitingMilliseconds", view.waitingMilliseconds},
                {"pausedMilliseconds", view.pausedMilliseconds}, {"attempts", Json::array()}};
            for (std::size_t attemptIndex = 0; attemptIndex < node.history.size(); ++attemptIndex)
            {
                const auto& attempt = node.history[attemptIndex];
                const auto& captured = view.attempts[attemptIndex];
                Json item = {{"id", captured.id}, {"ordinal", captured.ordinal}, {"stamp", StampJson(captured.stamp)},
                    {"state", captured.state}, {"executed", captured.executionSucceeded}, {"verified", captured.verified},
                    {"activeMilliseconds", captured.activeMilliseconds}, {"fingerprint", attempt.fingerprint},
                    {"diagnostic", attempt.privateDiagnostic}, {"prerequisites", Json::array()}};
                for (const auto& prerequisite : captured.prerequisites)
                    item["prerequisites"].push_back(ReferenceJson(prerequisite));
                if (captured.reportedTokens)
                    item["reportedTokens"] = *captured.reportedTokens;
                if (captured.reportedModel)
                    item["reportedModel"] = *captured.reportedModel;
                if (attempt.artifact)
                    item["artifact"] = {{"id", attempt.artifact->id}, {"version", attempt.artifact->version},
                        {"hash", attempt.artifact->hash}, {"content", attempt.artifact->content}};
                row["attempts"].push_back(std::move(item));
            }
            if (node.artifact)
                row["artifact"] = ReferenceJson({node.spec.id, node.artifact->id, node.artifact->version, node.artifact->hash});
            value["nodes"].push_back(std::move(row));
        }
        return value;
    }
    static WorkflowArtifact ReadArtifact(const Json& item)
    {
        WorkflowArtifact artifact{item.at("id"), item.at("version"), item.at("hash"), item.at("content")};
        if (!Identifier(artifact.id) || artifact.version == 0 || artifact.content.empty() || artifact.content.size() > MaximumMaterial ||
            !Digest(artifact.hash) || AgentWorkflow::ArtifactHash(artifact.content) != artifact.hash)
            throw std::runtime_error("artifact");
        return artifact;
    }
    static Node ReadNode(const Json& row, const WorkflowBudget& budget, const std::string& workflow, const std::string& companion,
        std::set<std::string>& attempts, std::uint64_t& reportedTokens, std::uint32_t& unreported)
    {
        Node node;
        node.spec = {row.at("id"), row.at("parent"), row.at("role").get<WorkflowRole>(), row.at("level"), row.at("label"),
            row.at("objective"), row.at("input"), row.at("evidence"), row.at("dependencies").get<std::vector<std::string>>(),
            row.at("readOnly")};
        node.view.id = node.spec.id;
        node.view.parentId = node.spec.parentId;
        node.view.role = node.spec.role;
        node.view.level = node.spec.level;
        node.view.dependencies = node.spec.dependsOn;
        node.view.state = row.at("state").get<WorkflowState>();
        node.view.activeMilliseconds = row.at("activeMilliseconds");
        node.view.waitingMilliseconds = row.at("waitingMilliseconds");
        node.view.pausedMilliseconds = row.at("pausedMilliseconds");
        if (ToString(node.view.state) == "Unknown" || node.view.activeMilliseconds > 86400000 ||
            node.view.waitingMilliseconds > 31536000000ULL || node.view.pausedMilliseconds > 31536000000ULL ||
            row.at("attempts").size() > budget.maximumAttemptsPerNode)
            throw std::runtime_error("state");
        for (const auto& item : row.at("attempts"))
        {
            Attempt attempt;
            auto& view = attempt.view;
            view.id = item.at("id");
            view.ordinal = item.at("ordinal");
            view.stamp = ReadStamp(item.at("stamp"));
            view.state = item.at("state").get<WorkflowState>();
            view.executionSucceeded = item.at("executed");
            view.verified = item.at("verified");
            view.activeMilliseconds = item.at("activeMilliseconds");
            attempt.fingerprint = item.at("fingerprint");
            attempt.privateDiagnostic = item.at("diagnostic");
            if (!Identifier(view.id) || !attempts.insert(view.id).second || view.ordinal != node.history.size() + 1 ||
                !StampValid(view.stamp, workflow) || view.stamp.companionId != companion || view.stamp.attemptId != view.id ||
                !Digest(attempt.fingerprint) || attempt.privateDiagnostic.size() > 1024 ||
                view.activeMilliseconds > node.view.activeMilliseconds ||
                (view.state != WorkflowState::Running && view.state != WorkflowState::Succeeded && view.state != WorkflowState::Failed &&
                    view.state != WorkflowState::Interrupted && view.state != WorkflowState::Cancelled))
                throw std::runtime_error("attempt");
            for (const auto& prerequisite : item.at("prerequisites"))
                view.prerequisites.push_back(ReadReference(prerequisite));
            if (view.prerequisites.size() != node.spec.dependsOn.size())
                throw std::runtime_error("prerequisites");
            if (item.contains("reportedTokens"))
            {
                view.reportedTokens = item.at("reportedTokens").get<std::uint64_t>();
                if (*view.reportedTokens > 1000000000)
                    throw std::runtime_error("tokens");
                reportedTokens += *view.reportedTokens;
            }
            if (item.contains("reportedModel"))
            {
                view.reportedModel = item.at("reportedModel").get<std::string>();
                if (!ReportedModel(*view.reportedModel))
                    throw std::runtime_error("model");
            }
            if (item.contains("artifact"))
            {
                attempt.artifact = ReadArtifact(item.at("artifact"));
                view.artifact = ArtifactReference{node.spec.id, attempt.artifact->id, attempt.artifact->version, attempt.artifact->hash};
            }
            if (view.verified != (view.executionSucceeded && view.state == WorkflowState::Succeeded && attempt.artifact.has_value()) ||
                (attempt.artifact && !view.verified))
                throw std::runtime_error("verification");
            if (view.state == WorkflowState::Running)
            {
                view.state = WorkflowState::Interrupted;
                ++unreported;
                view.diagnostic = "The process ended during this attempt; explicit read-only resume is required.";
            }
            else
                view.diagnostic = view.verified ? "Execution and artifact verification completed; parent acceptance remains separate."
                                                : "This attempt did not establish verified evidence.";
            node.history.push_back(std::move(attempt));
        }
        if (row.contains("artifact"))
        {
            const auto reference = ReadReference(row.at("artifact"));
            const auto found = std::find_if(node.history.begin(), node.history.end(),
                [&](const auto& attempt)
                {
                    return attempt.artifact && reference.nodeId == node.spec.id && reference.id == attempt.artifact->id &&
                           reference.version == attempt.artifact->version && reference.hash == attempt.artifact->hash;
                });
            if (found == node.history.end())
                throw std::runtime_error("current artifact");
            node.artifact = found->artifact;
        }
        if (node.view.state == WorkflowState::Running)
            node.view.state = WorkflowState::Interrupted;
        const bool retainsEvidence = node.view.state == WorkflowState::Succeeded ||
                                     (node.spec.role == WorkflowRole::Parent &&
                                         (node.view.state == WorkflowState::Accepted || node.view.state == WorkflowState::Rejected ||
                                             node.view.state == WorkflowState::Paused));
        if ((!retainsEvidence && node.artifact) || (node.view.state == WorkflowState::Succeeded && !node.artifact))
            throw std::runtime_error("current evidence");
        return node;
    }
    struct CheckpointData
    {
        WorkflowSpec spec;
        WorkflowSnapshot summary;
        std::vector<Node> nodes;
        bool parentEvaluation;
    };
    static CheckpointData Decode(const Json& value)
    {
        WorkflowSpec spec;
        WorkflowSnapshot summary;
        spec.id = value.at("id");
        summary.id = spec.id;
        summary.stamp = ReadStamp(value.at("stamp"));
        summary.state = value.at("state").get<WorkflowState>();
        summary.parentDecision = value.at("decision").get<ParentDecision>();
        summary.sequence = value.at("sequence");
        summary.requests = value.at("requests");
        summary.reportedTokens = value.at("reportedTokens");
        summary.unreportedRequests = value.at("unreportedRequests");
        const bool parentEvaluation = value.at("parentEvaluation");
        const auto& budget = value.at("budget");
        spec.budget = {budget.at("parallel"), budget.at("attempts"), budget.at("requests"), budget.at("activeMilliseconds"),
            budget.at("reportedTokens")};
        std::vector<Node> nodes;
        std::set<std::string> attempts;
        std::uint64_t reportedTokens = 0;
        for (const auto& row : value.at("nodes"))
        {
            auto node =
                ReadNode(row, spec.budget, spec.id, summary.stamp.companionId, attempts, reportedTokens, summary.unreportedRequests);
            spec.nodes.push_back(node.spec);
            nodes.push_back(std::move(node));
        }
        std::string validation;
        if (!Validate(spec, validation) || !StampValid(summary.stamp, spec.id) || ToString(summary.state) == "Unknown" ||
            ToString(summary.parentDecision) == "Unknown" || summary.requests != attempts.size() ||
            summary.requests > spec.budget.maximumRequests || summary.reportedTokens != reportedTokens ||
            summary.unreportedRequests > summary.requests)
            throw std::runtime_error("budget");
        for (const auto& node : nodes)
            for (const auto& attempt : node.history)
                for (std::size_t index = 0; index < attempt.view.prerequisites.size(); ++index)
                {
                    const auto& reference = attempt.view.prerequisites[index];
                    if (reference.nodeId != node.spec.dependsOn[index])
                        throw std::runtime_error("dependency order");
                    const auto& source =
                        *std::find_if(nodes.begin(), nodes.end(), [&](const auto& item) { return item.spec.id == reference.nodeId; });
                    if (!std::any_of(source.history.begin(), source.history.end(),
                            [&](const auto& previous)
                            {
                                return previous.artifact && previous.artifact->id == reference.id &&
                                       previous.artifact->version == reference.version && previous.artifact->hash == reference.hash;
                            }))
                        throw std::runtime_error("dependency artifact");
                }
        const auto& parent =
            *std::find_if(nodes.begin(), nodes.end(), [](const auto& node) { return node.spec.role == WorkflowRole::Parent; });
        const auto& reviewer =
            *std::find_if(nodes.begin(), nodes.end(), [](const auto& node) { return node.spec.role == WorkflowRole::Reviewer; });
        if ((summary.parentDecision == ParentDecision::Accept &&
                (summary.state != WorkflowState::Accepted || parent.view.state != WorkflowState::Accepted)) ||
            ((summary.state == WorkflowState::Accepted || summary.state == WorkflowState::AwaitingAcceptance) &&
                reviewer.view.state != WorkflowState::Succeeded))
            throw std::runtime_error("acceptance");
        if (summary.state == WorkflowState::Running)
            summary.state = WorkflowState::Paused;
        return {std::move(spec), std::move(summary), std::move(nodes), parentEvaluation};
    }
};

AgentWorkflow::AgentWorkflow() : impl(std::make_unique<Impl>())
{
}
AgentWorkflow::~AgentWorkflow()
{
    {
        std::lock_guard lock(impl->mutex);
        impl->observer = {};
        impl->notifications.clear();
    }
    RequestCancel();
    Join();
}
void AgentWorkflow::RequestCancel()
{
    std::stop_source source;
    {
        std::lock_guard lock(impl->mutex);
        source = impl->cancellation;
    }
    if (!source.request_stop())
        return;
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->nodes.empty() && impl->summary.parentDecision != ParentDecision::Accept &&
            impl->summary.parentDecision != ParentDecision::Reject)
        {
            impl->summary.state = WorkflowState::Cancelled;
            for (auto& node : impl->nodes)
                if (node.view.state == WorkflowState::Waiting || node.view.state == WorkflowState::AwaitingAcceptance ||
                    node.view.state == WorkflowState::Paused)
                    impl->Transition(node, WorkflowState::Cancelled);
            impl->ObserveLocked();
        }
    }
    impl->changed.notify_all();
    impl->Drain();
}
void AgentWorkflow::Join()
{
    {
        std::lock_guard lock(impl->mutex);
        if (impl->OwnedThread())
            return;
    }
    std::lock_guard control(impl->control);
    {
        std::lock_guard lock(impl->mutex);
        if (impl->OwnedThread())
            return;
    }
    if (impl->runner.joinable())
        impl->runner.join();
    std::lock_guard lock(impl->mutex);
    impl->runnerId = {};
}
WorkflowSnapshot AgentWorkflow::Snapshot() const
{
    std::lock_guard lock(impl->mutex);
    return impl->SnapshotLocked();
}
std::optional<WorkflowArtifact> AgentWorkflow::AcceptedArtifact() const
{
    std::lock_guard lock(impl->mutex);
    if (impl->summary.state != WorkflowState::Accepted || impl->summary.parentDecision != ParentDecision::Accept)
        return std::nullopt;
    const auto parent =
        std::find_if(impl->nodes.begin(), impl->nodes.end(), [](const auto& node) { return node.spec.role == WorkflowRole::Parent; });
    if (parent == impl->nodes.end() || !parent->artifact || parent->history.empty() || !parent->history.back().view.verified)
        return std::nullopt;
    return parent->artifact;
}
bool AgentWorkflow::Start(WorkflowSpec spec, runtime::RuntimeStamp stamp, Provider provider, std::string& error, Observer observer)
{
    if (!Validate(spec, error) || !StampValid(stamp, spec.id) || !provider)
    {
        if (error.empty())
            error = "Runtime origin and provider are required.";
        return false;
    }
    {
        std::lock_guard lock(impl->mutex);
        if (impl->OwnedThread())
        {
            error = "Execution callbacks cannot replace their owner.";
            return false;
        }
    }
    {
        std::lock_guard control(impl->control);
        {
            std::lock_guard lock(impl->mutex);
            if (impl->running || (!impl->nodes.empty() && impl->spec.id == spec.id))
            {
                error = "Resume preserves the existing workflow and its spent budgets.";
                return false;
            }
        }
        if (impl->runner.joinable())
            impl->runner.join();
        std::lock_guard lock(impl->mutex);
        impl->spec = std::move(spec);
        impl->summary = {};
        impl->summary.id = impl->spec.id;
        impl->summary.stamp = std::move(stamp);
        impl->nodes.clear();
        impl->jobs.clear();
        impl->jobIds.clear();
        impl->notifications.clear();
        for (const auto& node : impl->spec.nodes)
        {
            Impl::Node record;
            record.spec = node;
            record.view.id = node.id;
            record.view.parentId = node.parentId;
            record.view.role = node.role;
            record.view.level = node.level;
            record.view.dependencies = node.dependsOn;
            record.view.state = WorkflowState::Waiting;
            impl->nodes.push_back(std::move(record));
        }
        impl->provider = std::move(provider);
        impl->observer = std::move(observer);
        impl->cancellation = {};
        impl->active = 0;
        impl->running = true;
        impl->parentEvaluation = false;
        const auto era = ++impl->era;
        impl->ObserveLocked();
        impl->runner = std::jthread([this, era] { impl->Run(era); });
        impl->runnerId = impl->runner.get_id();
    }
    error.clear();
    impl->Drain();
    return true;
}
bool AgentWorkflow::Retry(const std::string& nodeId, std::string changedInput, std::string changedEvidence, std::string& error)
{
    {
        std::lock_guard lock(impl->mutex);
        if (impl->OwnedThread())
        {
            error = "Execution callbacks cannot restart their owner.";
            return false;
        }
    }
    {
        std::lock_guard control(impl->control);
        {
            std::lock_guard lock(impl->mutex);
            if (impl->running &&
                (impl->active != 0 || (impl->summary.state != WorkflowState::Paused && impl->summary.state != WorkflowState::Cancelled &&
                                          impl->summary.state != WorkflowState::Exhausted)))
            {
                error = "Active provider work must finish before recovery.";
                return false;
            }
        }
        // Paused is visible before thread teardown finishes; join outside the state lock.
        if (impl->runner.joinable())
            impl->runner.join();
        std::lock_guard lock(impl->mutex);
        impl->runnerId = {};
        const auto found = std::find_if(impl->nodes.begin(), impl->nodes.end(), [&](const auto& node) { return node.spec.id == nodeId; });
        if (impl->running || found == impl->nodes.end() || found->history.empty() ||
            impl->summary.parentDecision == ParentDecision::Accept || impl->summary.parentDecision == ParentDecision::Reject ||
            changedInput.size() > MaximumMaterial || changedEvidence.size() > 256 ||
            (found->view.state != WorkflowState::Failed && found->view.state != WorkflowState::Cancelled &&
                found->view.state != WorkflowState::Interrupted && impl->summary.parentDecision != ParentDecision::Rework &&
                impl->summary.parentDecision != ParentDecision::NeedEvidence))
        {
            error = "The node cannot be retried in its current state.";
            return false;
        }
        const auto fingerprint = InputFingerprint(changedInput, changedEvidence);
        if (fingerprint.empty() ||
            std::any_of(
                found->history.begin(), found->history.end(), [&](const auto& attempt) { return attempt.fingerprint == fingerprint; }) ||
            found->history.size() >= impl->spec.budget.maximumAttemptsPerNode ||
            impl->summary.requests >= impl->spec.budget.maximumRequests ||
            impl->summary.reportedTokens >= impl->spec.budget.maximumReportedTokens ||
            impl->ActiveTime() >= impl->spec.budget.maximumActiveMilliseconds)
        {
            error = "Recovery requires changed input or evidence within the original attempt budget.";
            return false;
        }
        found->spec.inputText = std::move(changedInput);
        found->spec.evidenceKey = std::move(changedEvidence);
        found->artifact.reset();
        for (auto& node : impl->spec.nodes)
            if (node.id == nodeId)
                node = found->spec;
        impl->Transition(*found, WorkflowState::Waiting);
        std::set<std::string> invalidated{nodeId};
        for (std::size_t pass = 0; pass < impl->nodes.size(); ++pass)
            for (auto& node : impl->nodes)
                if (std::any_of(
                        node.spec.dependsOn.begin(), node.spec.dependsOn.end(), [&](const auto& id) { return invalidated.contains(id); }))
                {
                    invalidated.insert(node.spec.id);
                    node.artifact.reset();
                    impl->Transition(node, WorkflowState::Waiting);
                }
        impl->summary.parentDecision = ParentDecision::Pending;
        impl->summary.state = WorkflowState::Paused;
        impl->ObserveLocked();
    }
    error.clear();
    impl->Drain();
    return true;
}
bool AgentWorkflow::Decide(const ParentDecision decision, std::string& error)
{
    {
        std::lock_guard lock(impl->mutex);
        if (impl->summary.state != WorkflowState::AwaitingAcceptance || impl->cancellation.stop_requested() ||
            decision == ParentDecision::Pending || ToString(decision) == "Unknown")
        {
            error = "Verified reviewer evidence is required before parent acceptance.";
            return false;
        }
        const auto state = decision == ParentDecision::Accept   ? WorkflowState::Accepted
                           : decision == ParentDecision::Reject ? WorkflowState::Rejected
                                                                : WorkflowState::Paused;
        impl->summary.parentDecision = decision;
        impl->summary.state = state;
        auto& parent =
            *std::find_if(impl->nodes.begin(), impl->nodes.end(), [](const auto& node) { return node.spec.role == WorkflowRole::Parent; });
        impl->Transition(parent, state);
        impl->ObserveLocked();
    }
    error.clear();
    impl->Drain();
    return true;
}
bool AgentWorkflow::Resume(runtime::RuntimeStamp newStamp, Provider provider, std::string& error, Observer observer)
{
    {
        std::lock_guard lock(impl->mutex);
        if (impl->OwnedThread())
        {
            error = "Execution callbacks cannot restart their owner.";
            return false;
        }
    }
    {
        std::lock_guard control(impl->control);
        {
            std::lock_guard lock(impl->mutex);
            if (impl->running || impl->nodes.empty() || !StampValid(newStamp, impl->spec.id) ||
                newStamp.companionId != impl->summary.stamp.companionId || !provider ||
                impl->summary.parentDecision == ParentDecision::Accept || impl->summary.parentDecision == ParentDecision::Reject)
            {
                error = "Explicit resume requires the same companion, a valid runtime origin and unfinished work.";
                return false;
            }
        }
        if (impl->runner.joinable())
            impl->runner.join();
        std::lock_guard lock(impl->mutex);
        impl->jobs.clear();
        impl->jobIds.clear();
        impl->summary.stamp = std::move(newStamp);
        impl->provider = std::move(provider);
        impl->observer = std::move(observer);
        impl->cancellation = {};
        impl->active = 0;
        impl->running = true;
        impl->parentEvaluation = false;
        for (auto& node : impl->nodes)
            if (node.view.state == WorkflowState::Interrupted || node.view.state == WorkflowState::Cancelled ||
                node.view.state == WorkflowState::Paused)
                impl->Transition(node, WorkflowState::Waiting);
        const auto& parent =
            *std::find_if(impl->nodes.begin(), impl->nodes.end(), [](const auto& node) { return node.spec.role == WorkflowRole::Parent; });
        impl->parentEvaluation = !parent.history.empty();
        const auto era = ++impl->era;
        impl->ObserveLocked();
        impl->runner = std::jthread([this, era] { impl->Run(era); });
        impl->runnerId = impl->runner.get_id();
    }
    error.clear();
    impl->Drain();
    return true;
}

bool AgentWorkflow::EvaluateParent(std::string& error)
{
    {
        std::lock_guard lock(impl->mutex);
        if (impl->OwnedThread())
        {
            error = "Schedule parent evaluation outside execution callbacks.";
            return false;
        }
    }
    {
        std::lock_guard control(impl->control);
        {
            std::lock_guard lock(impl->mutex);
            if (impl->running || impl->summary.state != WorkflowState::AwaitingAcceptance || !impl->provider ||
                impl->cancellation.stop_requested())
            {
                error = "Parent evaluation requires verified reviewer evidence and an idle workflow.";
                return false;
            }
            auto& parent = *std::find_if(
                impl->nodes.begin(), impl->nodes.end(), [](const auto& node) { return node.spec.role == WorkflowRole::Parent; });
            if (!parent.history.empty() || impl->summary.requests >= impl->spec.budget.maximumRequests ||
                impl->summary.reportedTokens >= impl->spec.budget.maximumReportedTokens ||
                impl->ActiveTime() >= impl->spec.budget.maximumActiveMilliseconds)
            {
                error = "Parent evaluation must remain within the original budgets; recovery requires changed evidence.";
                return false;
            }
        }
        if (impl->runner.joinable())
            impl->runner.join();
        std::lock_guard lock(impl->mutex);
        impl->jobs.clear();
        impl->jobIds.clear();
        impl->parentEvaluation = true;
        impl->running = true;
        auto& parent =
            *std::find_if(impl->nodes.begin(), impl->nodes.end(), [](const auto& node) { return node.spec.role == WorkflowRole::Parent; });
        impl->Transition(parent, WorkflowState::Waiting);
        const auto era = ++impl->era;
        impl->ObserveLocked();
        impl->runner = std::jthread([this, era] { impl->Run(era); });
        impl->runnerId = impl->runner.get_id();
    }
    error.clear();
    impl->Drain();
    return true;
}

bool AgentWorkflow::Save(const std::filesystem::path& path, std::string& error) const
{
    Json value;
    {
        std::lock_guard lock(impl->mutex);
        if (impl->nodes.empty())
        {
            error = "There is no workflow to save.";
            return false;
        }
        value = impl->EncodeLocked();
    }
    try
    {
        if (AtomicSave(path, value.dump()))
        {
            error.clear();
            return true;
        }
    }
    catch (...)
    {
    }
    error = "The workflow checkpoint could not be saved.";
    return false;
}

bool AgentWorkflow::Load(const std::filesystem::path& path, std::string& error)
{
    {
        std::lock_guard lock(impl->mutex);
        if (impl->OwnedThread())
        {
            error = "Execution callbacks cannot replace their owner.";
            return false;
        }
    }
    std::lock_guard control(impl->control);
    {
        std::lock_guard lock(impl->mutex);
        if (impl->running)
        {
            error = "A running workflow cannot be replaced.";
            return false;
        }
    }
    try
    {
        std::error_code fileError;
        const auto size = std::filesystem::file_size(path, fileError);
        if (fileError || size == 0 || size > MaximumCheckpoint)
            throw std::runtime_error("size");
        std::ifstream stream(path, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("read");
        const auto value = Json::parse(bytes);
        if (value.at("schema") != 1)
            throw std::runtime_error("schema");
        auto decoded = Impl::Decode(value);
        if (impl->runner.joinable())
            impl->runner.join();
        std::lock_guard lock(impl->mutex);
        impl->spec = std::move(decoded.spec);
        impl->summary = std::move(decoded.summary);
        impl->nodes = std::move(decoded.nodes);
        impl->jobs.clear();
        impl->jobIds.clear();
        impl->runnerId = {};
        impl->notifications.clear();
        impl->provider = {};
        impl->observer = {};
        impl->cancellation = {};
        impl->active = 0;
        impl->parentEvaluation = decoded.parentEvaluation;
        ++impl->era;
        error.clear();
        return true;
    }
    catch (...)
    {
        error = "The workflow checkpoint is invalid or unavailable.";
        return false;
    }
}

} // namespace revia::agents
