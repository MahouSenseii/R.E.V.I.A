#include "Agents/agentWorkflow.h"
#include "testSupport.h"

#include <atomic>
#include <fstream>
#include <nlohmann/json.hpp>

void RunAgentToolBudgetTests()
{
    using namespace revia::agents;
    using revia::tests::Check;
    revia::tests::ScopedTestDirectory directory;
    WorkflowSpec spec;
    spec.id = "tool-budget-fixture";
    spec.budget.maximumProviderCalls = 2;
    spec.budget.maximumToolCalls = 1;
    spec.budget.maximumToolOutputBytes = 1024;
    spec.nodes = {{"parent", {}, WorkflowRole::Parent, 3, "Parent", "Accept", {}, {}, {"review"}},
        {"first", "parent", WorkflowRole::Worker, 1, "First", "Inspect", {}, {}, {}, false},
        {"second", "parent", WorkflowRole::Worker, 1, "Second", "Inspect", {}, {}, {}, false},
        {"review", "parent", WorkflowRole::Reviewer, 1, "Review", "Review", {}, {}, {"first", "second"}}};
    AgentWorkflow workflow;
    const revia::runtime::RuntimeStamp origin{"companion", "session", 1, spec.id, {}, 1};
    std::atomic<unsigned> admittedTools{0}, admittedProviders{0};
    std::string error;
    Check(workflow.Start(
              spec, origin,
              [&](const NodeRequest& request, std::stop_token)
              {
                  Check(workflow.AttemptCurrent(request.stamp), "Provider began with no active attempt.");
                  auto forged = request.stamp;
                  forged.attemptId += "-forged";
                  Check(!workflow.ReserveWork(forged, WorkflowWorkKind::Tool, 1024), "Unknown attempt consumed tool authority.");
                  Check(
                      !workflow.ReserveWork(request.stamp, WorkflowWorkKind::Tool, 1025), "Oversized output reservation bypassed budget.");
                  if (workflow.ReserveWork(request.stamp, WorkflowWorkKind::Provider))
                      ++admittedProviders;
                  if (workflow.ReserveWork(request.stamp, WorkflowWorkKind::Tool, 1024))
                      ++admittedTools;
                  return NodeResult{};
              },
              error),
        error);
    workflow.Join();
    const auto snapshot = workflow.Snapshot();
    Check(admittedProviders == 2 && admittedTools == 1 && snapshot.providerCalls == 2 && snapshot.toolCalls == 1 &&
              snapshot.toolOutputBytes == 1024,
        "Concurrent workers did not share cumulative work reservations.");
    Check(!workflow.AttemptCurrent(snapshot.nodes[1].attempts.back().stamp) &&
              !workflow.ReserveWork(snapshot.nodes[1].attempts.back().stamp, WorkflowWorkKind::Provider),
        "Completed attempt retained work admission.");
    const auto checkpoint = directory.root / "workflow.json";
    Check(workflow.Save(checkpoint, error), error);
    AgentWorkflow reopened;
    Check(reopened.Load(checkpoint, error), error);
    const auto restored = reopened.Snapshot();
    Check(restored.providerCalls == 2 && restored.toolCalls == 1 && restored.toolOutputBytes == 1024,
        "Restart discarded spent tool/provider reservations.");
    Check(reopened.Retry("first", "new evidence", "v2", error), error);
    Check(reopened.Resume(
              origin,
              [&](const NodeRequest& request, std::stop_token)
              {
                  Check(!reopened.ReserveWork(request.stamp, WorkflowWorkKind::Provider) &&
                            !reopened.ReserveWork(request.stamp, WorkflowWorkKind::Tool, 1024),
                      "Retry replenished cumulative tool budget.");
                  return NodeResult{};
              },
              error),
        error);
    reopened.Join();
    std::ifstream input(checkpoint);
    auto document = nlohmann::json::parse(input);
    document["toolCalls"] = 0;
    std::ofstream(checkpoint, std::ios::trunc) << document.dump();
    AgentWorkflow corrupt;
    Check(!corrupt.Load(checkpoint, error), "Checkpoint accepted summary/attempt work-counter disagreement.");

    spec.nodes[2].dependsOn = {"first"};
    std::atomic<bool> verifiedAfterArtifact{false};
    AgentWorkflow ordered;
    Check(ordered.Start(
              spec, origin,
              [&](const NodeRequest& request, std::stop_token)
              {
                  if (request.node.id == "second")
                      verifiedAfterArtifact = request.prerequisites.size() == 1 && request.prerequisiteReferences.size() == 1 &&
                                              request.prerequisiteReferences.front().nodeId == "first";
                  NodeResult result;
                  result.succeeded = result.verified = true;
                  result.artifact = {request.node.id + "-evidence", 1, {}, "Host fixture evidence"};
                  result.artifact.hash = AgentWorkflow::ArtifactHash(result.artifact.content);
                  return result;
              },
              error),
        error);
    ordered.Join();
    Check(verifiedAfterArtifact && ordered.Snapshot().state == WorkflowState::AwaitingAcceptance,
        "Tool verification ran before the producing worker's admitted artifact.");
    spec.nodes[2].readOnly = true;
    Check(!AgentWorkflow::Validate(spec, error), "Analytical workflow silently accepted tool-worker dependency semantics.");

    spec.nodes[2].readOnly = false;
    spec.budget.maximumReportedTokens = 1;
    spec.budget.maximumProviderCalls = 10;
    spec.budget.maximumToolCalls = 10;
    spec.budget.maximumToolOutputBytes = 10240;
    AgentWorkflow tokens;
    std::atomic<bool> chargedAndStopped{false};
    Check(tokens.Start(
              spec, origin,
              [&](const NodeRequest& request, std::stop_token)
              {
                  if (request.node.id == "first")
                  {
                      Check(tokens.ReserveWork(request.stamp, WorkflowWorkKind::Provider), "First token fixture request refused.");
                      Check(tokens.ChargeReportedTokens(request.stamp, 2), "Actual per-call usage was not charged.");
                      chargedAndStopped = !tokens.ReserveWork(request.stamp, WorkflowWorkKind::Provider) &&
                                          !tokens.ReserveWork(request.stamp, WorkflowWorkKind::Tool, 1);
                  }
                  NodeResult result;
                  result.reportedTokens = 2;
                  return result;
              },
              error),
        error);
    tokens.Join();
    Check(chargedAndStopped && tokens.Snapshot().reportedTokens == 2, "Per-call budget exhaustion was delayed or counted twice.");

    spec.budget.maximumReportedTokens = 100;
    const auto interruptedPath = directory.root / "interrupted-tools.json";
    AgentWorkflow interrupted;
    Check(interrupted.Start(
              spec, origin,
              [&](const NodeRequest& request, std::stop_token)
              {
                  if (request.node.id == "first")
                  {
                      Check(interrupted.ReserveWork(request.stamp, WorkflowWorkKind::Tool, 1),
                          "Fixture did not reserve an uncertain effect.");
                      std::string saveError;
                      Check(interrupted.Save(interruptedPath, saveError), saveError);
                  }
                  return NodeResult{};
              },
              error),
        error);
    interrupted.Join();
    AgentWorkflow recovery;
    Check(recovery.Load(interruptedPath, error), error);
    std::atomic<unsigned> recoveryCalls{0};
    const auto provider = [&](const NodeRequest&, std::stop_token)
    {
        ++recoveryCalls;
        return NodeResult{};
    };
    Check(!recovery.Resume(origin, provider, error) && recoveryCalls == 0,
        "Restart replayed an uncertain tool effect without observed recovery evidence.");
    Check(!recovery.Retry("first", "changed input only", "", error), "Changing an objective bypassed uncertain effect recovery.");
    Check(recovery.Retry(
              "first", "Inspect actual effects before further work", "Owner observed the completed write and requests recovery", error),
        error);
    Check(recovery.Resume(origin, provider, error), error);
    recovery.Join();
    Check(recoveryCalls == 1 && recovery.Snapshot().toolCalls == 1,
        "Explicit recovery reset spent tool budget or did not resume the selected worker.");
}
