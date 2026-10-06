#pragma once

#include "Runtime/runtimeStamp.h"
#include "Agents/agentDeliverable.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::agents
{

enum class WorkflowRole
{
    Parent,
    Worker,
    Reviewer
};
enum class WorkflowState
{
    Queued,
    Waiting,
    Running,
    Paused,
    Succeeded,
    Failed,
    Interrupted,
    Cancelled,
    Exhausted,
    AwaitingAcceptance,
    Accepted,
    Rejected
};
enum class ParentDecision
{
    Pending,
    Accept,
    Rework,
    Reject,
    NeedEvidence
};

[[nodiscard]] std::string ToString(WorkflowRole value);
[[nodiscard]] std::string ToString(WorkflowState value);
[[nodiscard]] std::string ToString(ParentDecision value);

struct WorkflowBudget
{
    std::uint32_t maximumParallel = 2;
    std::uint32_t maximumAttemptsPerNode = 2;
    std::uint32_t maximumRequests = 8;
    std::uint64_t maximumActiveMilliseconds = 120000;
    std::uint64_t maximumReportedTokens = 20000;
    std::uint32_t maximumProviderCalls = 32;
    std::uint32_t maximumToolCalls = 16;
    std::uint64_t maximumToolOutputBytes = 262144;
};

enum class WorkflowWorkKind
{
    Provider,
    Tool
};

struct WorkflowNode
{
    std::string id;
    std::string parentId;
    WorkflowRole role = WorkflowRole::Worker;
    int level = 1;
    // Runtime-approved safe presentation label; provider material stays out of snapshots.
    std::string label;
    std::string objective;
    std::string inputText;
    std::string evidenceKey;
    std::vector<std::string> dependsOn;
    bool readOnly = true;
    DeliverableContract deliverableContract;
};

struct WorkflowSpec
{
    std::string id;
    std::vector<WorkflowNode> nodes;
    WorkflowBudget budget;
};

struct WorkflowArtifact
{
    std::string id;
    std::uint64_t version = 0;
    std::string hash;
    std::string content;
};

struct ArtifactReference
{
    std::string nodeId;
    std::string id;
    std::uint64_t version = 0;
    std::string hash;
};

struct NodeRequest
{
    WorkflowNode node;
    runtime::RuntimeStamp stamp;
    std::uint32_t attempt = 0;
    std::vector<WorkflowArtifact> prerequisites;
    std::vector<ArtifactReference> prerequisiteReferences;
};

struct NodeResult
{
    bool succeeded = false;
    bool verified = false;
    WorkflowArtifact artifact;
    // Provider-only diagnostic material is persisted privately, never shown in snapshots.
    std::string diagnostic;
    std::optional<std::uint64_t> reportedTokens;
    std::optional<std::string> reportedModel;
};

struct WorkflowAttempt
{
    std::string id;
    std::uint32_t ordinal = 0;
    runtime::RuntimeStamp stamp;
    WorkflowState state = WorkflowState::Queued;
    bool executionSucceeded = false;
    bool verified = false;
    std::vector<ArtifactReference> prerequisites;
    std::optional<ArtifactReference> artifact;
    std::uint64_t activeMilliseconds = 0;
    std::optional<std::uint64_t> reportedTokens;
    std::optional<std::string> reportedModel;
    std::string diagnostic;
    std::uint32_t providerCalls = 0;
    std::uint32_t toolCalls = 0;
    std::uint64_t toolOutputBytes = 0;
    std::uint64_t chargedTokens = 0;
};

struct WorkflowNodeSnapshot
{
    std::string id;
    std::string parentId;
    WorkflowRole role = WorkflowRole::Worker;
    int level = 1;
    WorkflowState state = WorkflowState::Queued;
    std::string workingOn;
    std::string currentAction;
    std::string waitingFor;
    std::string nextHandoff;
    std::vector<std::string> dependencies;
    std::vector<WorkflowAttempt> attempts;
    std::uint64_t activeMilliseconds = 0;
    std::uint64_t waitingMilliseconds = 0;
    std::uint64_t pausedMilliseconds = 0;
};

struct WorkflowSnapshot
{
    std::string id;
    runtime::RuntimeStamp stamp;
    WorkflowState state = WorkflowState::Queued;
    ParentDecision parentDecision = ParentDecision::Pending;
    std::uint64_t sequence = 0;
    std::uint32_t requests = 0;
    std::uint64_t reportedTokens = 0;
    std::uint32_t unreportedRequests = 0;
    std::uint32_t providerCalls = 0;
    std::uint32_t toolCalls = 0;
    std::uint64_t toolOutputBytes = 0;
    std::vector<WorkflowNodeSnapshot> nodes;
};

// One bounded workflow; Runtime owns its provider, authority and presentation adapter.
class AgentWorkflow
{
  public:
    // Providers must honor cancellation; observers run outside state locks and keep this owner alive.
    using Provider = std::function<NodeResult(const NodeRequest&, std::stop_token)>;
    using Observer = std::function<void(const WorkflowSnapshot&)>;

    AgentWorkflow();
    ~AgentWorkflow();
    AgentWorkflow(const AgentWorkflow&) = delete;
    AgentWorkflow& operator=(const AgentWorkflow&) = delete;

    [[nodiscard]] static bool Validate(const WorkflowSpec& spec, std::string& error);
    [[nodiscard]] static std::string ArtifactHash(const std::string& content);
    bool Start(WorkflowSpec spec, runtime::RuntimeStamp stamp, Provider provider, std::string& error, Observer observer = {});
    void RequestCancel();
    void Join();
    [[nodiscard]] WorkflowSnapshot Snapshot() const;
    [[nodiscard]] std::optional<WorkflowArtifact> AcceptedArtifact() const;
    bool Retry(const std::string& nodeId, std::string changedInput, std::string changedEvidence, std::string& error);
    bool EvaluateParent(std::string& error);
    bool Decide(ParentDecision decision, std::string& error);
    bool Save(const std::filesystem::path& path, std::string& error) const;
    bool Load(const std::filesystem::path& path, std::string& error);
    bool Resume(runtime::RuntimeStamp newStamp, Provider provider, std::string& error, Observer observer = {});
    [[nodiscard]] bool AttemptCurrent(const runtime::RuntimeStamp& stamp) const;
    // Reservations remain spent after cancellation/failure. Runtime persists before dispatch.
    [[nodiscard]] bool ReserveWork(const runtime::RuntimeStamp& stamp, WorkflowWorkKind kind, std::uint64_t outputBytes = 0);
    [[nodiscard]] bool ChargeReportedTokens(const runtime::RuntimeStamp& stamp, std::uint64_t tokens);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace revia::agents
