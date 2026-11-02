#include "Agents/agentWorkflow.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

namespace
{
using namespace revia::agents;
using namespace std::chrono_literals;

void Check(const bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

WorkflowSpec Spec()
{
    WorkflowSpec spec;
    spec.id = "fixture-workflow";
    spec.nodes = {{"parent", {}, WorkflowRole::Parent, 3, "Parent acceptance", "Accept reviewed evidence", {}, {}, {"reviewer"}},
        {"first", "parent", WorkflowRole::Worker, 1, "First worker", "Inspect first fixture", "first input", "evidence-1", {}},
        {"second", "parent", WorkflowRole::Worker, 1, "Second worker", "Inspect second fixture", "second input", "evidence-1", {}},
        {"reviewer", "parent", WorkflowRole::Reviewer, 1, "Independent reviewer", "Review both artifacts", "review input", "evidence-1",
            {"first", "second"}}};
    return spec;
}

revia::runtime::RuntimeStamp Stamp()
{
    return {"fixture-companion", "fixture-session", 1, "fixture-workflow", {}, 1};
}

const WorkflowNodeSnapshot& Node(const WorkflowSnapshot& snapshot, const std::string& id)
{
    for (const auto& node : snapshot.nodes)
        if (node.id == id)
            return node;
    throw std::runtime_error("The requested node is missing.");
}

NodeResult Verified(const NodeRequest& request)
{
    NodeResult result;
    result.succeeded = true;
    result.verified = true;
    result.artifact = {request.node.id + "-artifact", request.attempt, {}, "Verified fixture evidence for " + request.node.id};
    result.artifact.hash = AgentWorkflow::ArtifactHash(result.artifact.content);
    return result;
}

std::filesystem::path Checkpoint(const std::string& name)
{
    const auto directory = std::filesystem::absolute(__FILE__).parent_path().parent_path() / "build/studio-20261003-m2-m3/worker/fixtures";
    std::filesystem::create_directories(directory);
    return directory / (name + ".json");
}

std::string CheckpointBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void TestCheckpointSaveFailureDetails()
{
    AgentWorkflow workflow;
    std::string error;
    Check(workflow.Start(
              Spec(), Stamp(), [](const NodeRequest& request, std::stop_token) { return Verified(request); }, error),
        "Save failure fixture did not start.");
    workflow.Join();
    Check(!workflow.Save({}, error) && error.find("empty") != std::string::npos,
        "An empty checkpoint path did not explain the refusal.");
    const auto blocked = Checkpoint("blocked-parent");
    {
        std::ofstream file(blocked, std::ios::binary | std::ios::trunc);
        file << "owned parent blocker";
    }
    Check(!workflow.Save(blocked / "checkpoint.json", error) && error.find("create_directories") != std::string::npos &&
              error.find("error") != std::string::npos && error.find(blocked.string()) == std::string::npos,
        "A parent creation failure lost its operation/code or exposed the private path.");
    const auto directory = Checkpoint("blocked-publication");
    std::filesystem::create_directories(directory);
    Check(!workflow.Save(directory, error) &&
#ifdef _WIN32
              error.find("MoveFileExW") != std::string::npos && error.find("Win32 error") != std::string::npos &&
#else
              error.find("rename") != std::string::npos && error.find("errno") != std::string::npos &&
#endif
              error.find(directory.string()) == std::string::npos,
        "A publication failure lost its primary native operation/code.");
    Check(std::filesystem::is_directory(directory), "A failed publication replaced its existing destination.");
    const auto supported = Checkpoint("save-error-cleared");
    Check(workflow.Save(supported, error) && error.empty(), "A supported save retained a stale failure.");
    Check(workflow.EvaluateParent(error), "Save overwrite parent evaluation was refused.");
    workflow.Join();
    error = "stale save error";
    Check(workflow.Save(supported, error) && error.empty(), "A supported overwrite failed or retained a stale error.");
    AgentWorkflow restored;
    Check(restored.Load(supported, error) && restored.Snapshot().requests == workflow.Snapshot().requests &&
              Node(restored.Snapshot(), "parent").attempts.size() == 1,
        "A supported overwrite did not persist the changed workflow.");
}

void TestCheckpointPendingPathRefusalPreservesPriorCheckpoint()
{
    AgentWorkflow workflow;
    std::string error;
    Check(workflow.Start(
              Spec(), Stamp(), [](const NodeRequest& request, std::stop_token) { return Verified(request); }, error),
        "Deep checkpoint fixture did not start.");
    workflow.Join();
    const auto supported = Checkpoint("prior-valid-checkpoint");
    Check(workflow.Save(supported, error), "The prior regular checkpoint could not be prepared.");
    const auto prior = CheckpointBytes(supported);
    const auto directory = supported.parent_path();
    const auto prefixLength = (directory / "x").native().size() - 1;
    const auto destinationLength = std::max(std::size_t{236}, prefixLength + 6);
    const auto destination = directory / (std::string(destinationLength - prefixLength - 5, 'd') + ".json");
    {
        std::ofstream file(destination, std::ios::binary | std::ios::trunc);
        file << prior;
        Check(file.good(), "The final-name-fit checkpoint fixture could not be prepared.");
    }
    Check(CheckpointBytes(destination) == prior && std::filesystem::is_regular_file(destination),
        "The prior valid regular checkpoint was not present at the deep destination.");
    Check(workflow.EvaluateParent(error), "Deep checkpoint parent evaluation was refused.");
    workflow.Join();
    const bool saved = workflow.Save(destination, error);
    const auto saveError = error;
    AgentWorkflow restored;
    if (saved)
    {
        Check(error.empty() && restored.Load(destination, error) && restored.Snapshot().requests == workflow.Snapshot().requests &&
                  Node(restored.Snapshot(), "parent").attempts.size() == 1,
            "A natively supported deep save did not round-trip the changed workflow.");
    }
    else
    {
        Check(CheckpointBytes(destination) == prior && restored.Load(destination, error) &&
                  restored.Snapshot().requests + 1 == workflow.Snapshot().requests &&
                  Node(restored.Snapshot(), "parent").attempts.empty(),
            "A refused pending path damaged or invalidated the earlier regular checkpoint.");
#ifdef _WIN32
        Check(saveError.find("CreateFileW") != std::string::npos && saveError.find("Win32 error") != std::string::npos &&
                  saveError.find("UTF-16") != std::string::npos && saveError.find("shorter writable runtime location") != std::string::npos,
            "A native pending-path refusal did not expose actionable length context.");
#endif
        Check(saveError.find(destination.string()) == std::string::npos && saveError.find("fixture-workflow") == std::string::npos,
            "Checkpoint failure text exposed a private path or serialized workflow material.");
    }
}

void TestOversizeCheckpointSaveFailure()
{
    auto spec = Spec();
    for (auto& node : spec.nodes)
    {
        node.objective = std::string(8192, '\x01');
        node.inputText = std::string(8192, '\x01');
    }
    AgentWorkflow workflow;
    std::string error;
    Check(workflow.Start(
              spec, Stamp(),
              [](const NodeRequest& request, std::stop_token)
              {
                  auto result = Verified(request);
                  result.artifact.content = std::string(8192, '\x01');
                  result.artifact.hash = AgentWorkflow::ArtifactHash(result.artifact.content);
                  return result;
              },
              error),
        "Oversize checkpoint fixture did not start.");
    workflow.Join();
    Check(!workflow.Save(Checkpoint("oversize-refusal"), error) && error.find("524288") != std::string::npos,
        "An oversized serialized checkpoint did not explain its size limit.");
}

void TestIndependentWorkersOverlap()
{
    std::mutex mutex;
    std::condition_variable condition;
    int entered = 0;
    bool release = false;
    AgentWorkflow workflow;
    std::string error;
    const bool started = workflow.Start(
        Spec(), Stamp(),
        [&](const NodeRequest& request, std::stop_token token)
        {
            if (request.node.role == WorkflowRole::Worker)
            {
                std::unique_lock lock(mutex);
                ++entered;
                condition.notify_all();
                while (!release && !token.stop_requested())
                    condition.wait_for(lock, 5ms);
            }
            return NodeResult{};
        },
        error);
    bool overlapped;
    {
        std::unique_lock lock(mutex);
        overlapped = condition.wait_for(lock, 500ms, [&] { return entered == 2; });
        release = true;
        condition.notify_all();
    }
    workflow.RequestCancel();
    workflow.Join();
    Check(started && overlapped, "Independent workflow workers did not overlap before gate release.");
}

void TestAcceptanceRequiresParentArtifact()
{
    AgentWorkflow workflow;
    std::string error;
    Check(workflow.Start(
              Spec(), Stamp(), [](const NodeRequest& request, std::stop_token) { return Verified(request); }, error),
        "The parent acceptance regression did not start.");
    workflow.Join();
    Check(!workflow.Decide(ParentDecision::Accept, error) && !workflow.AcceptedArtifact(),
        "A workflow was accepted without a verified parent deliverable.");
}

void TestClaimedVerificationRequiresDeliverableSections()
{
    auto spec = Spec();
    spec.nodes[1].deliverableContract.requirements = {{DeliverableSection::Steps, "Give actual ordered steps for this task.", false}};
    AgentWorkflow workflow;
    std::string error;
    std::atomic<int> reviewerCalls{0};
    Check(workflow.Start(
              spec, Stamp(),
              [&](const NodeRequest& request, std::stop_token)
              {
                  if (request.node.role == WorkflowRole::Reviewer)
                      ++reviewerCalls;
                  auto result = Verified(request);
                  result.artifact.content = R"({"summary":"A three-step plan is provided.","evidence":"The objective supports the plan."})";
                  result.artifact.hash = AgentWorkflow::ArtifactHash(result.artifact.content);
                  return result;
              },
              error),
        "The incomplete deliverable regression did not start.");
    workflow.Join();
    Check(Node(workflow.Snapshot(), "first").state == WorkflowState::Failed && reviewerCalls == 0,
        "A provider's verification assertion admitted a deliverable with no required steps.");
}

void TestDeliverableBoundsAndExactEvidence()
{
    const DeliverableContract contract{{{DeliverableSection::Steps, "List actual steps.", false},
                                           {DeliverableSection::Risks, "Identify relevant risk or explain why none is known.", true}},
        true};
    const std::vector<ArtifactReference> prerequisites{{"first", "first-artifact", 1, std::string(64, 'a')}};
    nlohmann::json payload = {{"summary", "Read the admitted marker."}, {"evidence", "The first artifact proposes a read."},
        {"steps", {{"items", {"Read the explicit marker file through the existing policy."}}, {"noneReason", ""}}},
        {"risks", {{"items", nlohmann::json::array()},
                      {"noneReason", "Only a bounded read is proposed; no write or external effect is requested."}}},
        {"prerequisiteEvidence", {{{"nodeId", "first"}, {"id", "first-artifact"}, {"version", 1}, {"hash", std::string(64, 'a')}}}}};
    std::string error;
    Check(ValidateDeliverable(contract, payload.dump(), prerequisites, error), "A complete bounded deliverable was rejected.");
    const auto valid = payload;
    payload["steps"]["items"] = nlohmann::json::array();
    payload["steps"]["noneReason"] = "No steps are known.";
    Check(!ValidateDeliverable(contract, payload.dump(), prerequisites, error), "Required steps were replaced by an absence claim.");
    payload = valid;
    payload["risks"]["noneReason"] = " \t\n";
    Check(!ValidateDeliverable(contract, payload.dump(), prerequisites, error), "A blank no-known-risk rationale was accepted.");
    payload["risks"]["noneReason"] = "No known risks.";
    Check(!ValidateDeliverable(contract, payload.dump(), prerequisites, error), "An absence label was accepted without a rationale.");
    payload = valid;
    payload["steps"]["items"][0] = std::string(1025, 'x');
    Check(!ValidateDeliverable(contract, payload.dump(), prerequisites, error), "An oversized section item was accepted.");
    payload = valid;
    payload["steps"]["items"] = std::vector<std::string>(9, "A proposed read.");
    Check(!ValidateDeliverable(contract, payload.dump(), prerequisites, error), "A section exceeded its bounded item count.");
    for (const auto& field : {"nodeId", "id", "hash"})
    {
        payload = valid;
        payload["prerequisiteEvidence"][0][field] = "not-supplied";
        Check(!ValidateDeliverable(contract, payload.dump(), prerequisites, error), "A fabricated prerequisite identity was accepted.");
    }
    payload = valid;
    payload["prerequisiteEvidence"][0]["version"] = 2;
    Check(!ValidateDeliverable(contract, payload.dump(), prerequisites, error), "A stale prerequisite version was accepted.");
    payload = valid;
    payload["prerequisiteEvidence"].push_back(payload["prerequisiteEvidence"][0]);
    Check(!ValidateDeliverable(contract, payload.dump(), prerequisites, error), "A duplicate prerequisite was accepted.");
    payload = valid;
    payload["prerequisiteEvidence"] = nlohmann::json::array();
    Check(!ValidateDeliverable(contract, payload.dump(), prerequisites, error), "Required prerequisite evidence was omitted.");
}

void TestCompleteDeliverableSurvivesCheckpointValidation()
{
    auto spec = Spec();
    for (auto& node : spec.nodes)
        node.deliverableContract = {{{DeliverableSection::Steps, "Give actual read-only steps.", false}}, true};
    AgentWorkflow workflow;
    std::string error;
    Check(workflow.Start(
              spec, Stamp(),
              [](const NodeRequest& request, std::stop_token)
              {
                  auto result = Verified(request);
                  nlohmann::json payload = {{"summary", "A bounded read-only plan."}, {"evidence", "Only supplied evidence is used."},
                      {"steps", {{"items", {"Read the approved marker and report its actual contents."}}, {"noneReason", ""}}},
                      {"prerequisiteEvidence", nlohmann::json::array()}};
                  for (const auto& reference : request.prerequisiteReferences)
                      payload["prerequisiteEvidence"].push_back(
                          {{"nodeId", reference.nodeId}, {"id", reference.id}, {"version", reference.version}, {"hash", reference.hash}});
                  result.artifact.content = payload.dump();
                  result.artifact.hash = AgentWorkflow::ArtifactHash(result.artifact.content);
                  return result;
              },
              error),
        "The complete contract workflow did not start.");
    workflow.Join();
    Check(workflow.EvaluateParent(error), "Complete evidence did not reach separate parent review.");
    workflow.Join();
    Check(workflow.Decide(ParentDecision::Accept, error) && workflow.AcceptedArtifact(), "Complete reviewed parent work was not accepted.");
    const auto path = Checkpoint("typed-deliverable");
    Check(workflow.Save(path, error), "Typed deliverable checkpoint was not saved.");
    AgentWorkflow restored;
    Check(restored.Load(path, error) && restored.AcceptedArtifact(), "Typed deliverable did not survive restart.");
    nlohmann::json checkpoint;
    {
        std::ifstream input(path);
        input >> checkpoint;
    }
    auto content = nlohmann::json::parse(checkpoint["nodes"][0]["attempts"][0]["artifact"]["content"].get<std::string>());
    content["steps"]["items"] = nlohmann::json::array();
    const auto changed = content.dump();
    checkpoint["nodes"][0]["attempts"][0]["artifact"]["content"] = changed;
    checkpoint["nodes"][0]["attempts"][0]["artifact"]["hash"] = AgentWorkflow::ArtifactHash(changed);
    checkpoint["nodes"][0]["artifact"]["hash"] = AgentWorkflow::ArtifactHash(changed);
    {
        std::ofstream output(path);
        output << checkpoint.dump();
    }
    Check(!restored.Load(path, error) && restored.AcceptedArtifact(), "A recomputed hash bypassed checkpoint deliverable completeness.");
    checkpoint["schema"] = 1;
    {
        std::ofstream output(path);
        output << checkpoint.dump();
    }
    const auto bytes = checkpoint.dump();
    Check(!restored.Load(path, error) && error.find("previous weak acceptance") != std::string::npos,
        "An old weak checkpoint was silently trusted.");
    nlohmann::json retained;
    {
        std::ifstream input(path);
        input >> retained;
    }
    Check(retained.dump() == bytes, "Refusing an old checkpoint changed its stored file.");
}

void TestDependencyWaitAndSeparateParentAcceptance()
{
    std::mutex mutex;
    std::condition_variable condition;
    int entered = 0;
    bool release = false;
    std::atomic<int> reviewed = 0, parentCalls = 0;
    std::atomic<bool> references = true;
    AgentWorkflow workflow;
    std::string error;
    Check(workflow.Start(
              Spec(), Stamp(),
              [&](const NodeRequest& request, std::stop_token token)
              {
                  if (request.node.role == WorkflowRole::Worker)
                  {
                      std::unique_lock lock(mutex);
                      ++entered;
                      condition.notify_all();
                      while (!release && !token.stop_requested())
                          condition.wait_for(lock, 5ms);
                  }
                  else if (request.node.role == WorkflowRole::Reviewer)
                  {
                      ++reviewed;
                      references = request.prerequisites.size() == 2 && request.prerequisites[0].version == 1 &&
                                   request.prerequisites[1].version == 1 &&
                                   request.prerequisites[0].hash == AgentWorkflow::ArtifactHash(request.prerequisites[0].content);
                  }
                  else
                  {
                      ++parentCalls;
                      references = references && request.prerequisites.size() == 1 && request.prerequisites[0].id == "reviewer-artifact";
                  }
                  return Verified(request);
              },
              error, [&](const WorkflowSnapshot&) { static_cast<void>(workflow.Snapshot()); }),
        "The real workflow did not start.");
    bool overlap, reviewerWaited;
    {
        std::unique_lock lock(mutex);
        overlap = condition.wait_for(lock, 1s, [&] { return entered == 2; });
        condition.wait_for(lock, 25ms);
        reviewerWaited = reviewed == 0;
        release = true;
        condition.notify_all();
    }
    workflow.Join();
    Check(overlap && reviewerWaited && reviewed == 1 && references, "Reviewer did not wait for both real worker artifacts.");
    auto snapshot = workflow.Snapshot();
    Check(snapshot.state == WorkflowState::AwaitingAcceptance && snapshot.parentDecision == ParentDecision::Pending && parentCalls == 0,
        "Provider execution automatically accepted or dispatched the parent.");
    Check(!workflow.AcceptedArtifact(), "A deliverable was exposed before parent acceptance.");
    Check(Node(snapshot, "reviewer").waitingMilliseconds >= 20 && Node(snapshot, "first").activeMilliseconds >= 20,
        "Measured active and dependency waiting durations were not recorded.");
    Check(workflow.EvaluateParent(error), "Explicit parent provider evaluation was refused.");
    workflow.Join();
    snapshot = workflow.Snapshot();
    Check(parentCalls == 1 && snapshot.requests == 4 && Node(snapshot, "parent").attempts.front().verified && references &&
              snapshot.state == WorkflowState::AwaitingAcceptance && snapshot.parentDecision == ParentDecision::Pending,
        "Parent evaluation was not real, budgeted and separate from acceptance.");
    Check(!workflow.EvaluateParent(error), "An unchanged parent evaluation was repeated.");
    Check(workflow.Decide(ParentDecision::Accept, error) && workflow.Snapshot().state == WorkflowState::Accepted,
        "Explicit parent acceptance did not complete the workflow.");
    Check(workflow.AcceptedArtifact() && workflow.AcceptedArtifact()->id == "parent-artifact",
        "Accepted parent work did not expose its verified deliverable.");
}

void TestExpectedFailureAndChangedEvidenceRecovery()
{
    AgentWorkflow workflow;
    std::string error;
    std::atomic<int> reviewerCalls = 0;
    const auto provider = [&](const NodeRequest& request, std::stop_token)
    {
        auto result = Verified(request);
        if (request.node.id == "second" && request.attempt == 1)
        {
            result.verified = false;
            result.diagnostic = "PRIVATE_SENTINEL failed diagnostic despite process exit zero";
        }
        if (request.node.role == WorkflowRole::Reviewer)
            ++reviewerCalls;
        return result;
    };
    Check(workflow.Start(Spec(), Stamp(), provider, error), "Failure fixture did not start.");
    workflow.Join();
    const auto failed = workflow.Snapshot();
    Check(Node(failed, "second").attempts.front().executionSucceeded && !Node(failed, "second").attempts.front().verified &&
              Node(failed, "second").state == WorkflowState::Failed && reviewerCalls == 0 &&
              !workflow.Decide(ParentDecision::Accept, error),
        "Execution success was treated as verified evidence.");
    Check(!workflow.Retry("second", "second input", "evidence-1", error), "Identical failed input was retried.");
    Check(workflow.Retry("second", "second input", "changed-diagnostic", error), "Changed diagnostic recovery was refused.");
    auto stamp = Stamp();
    stamp.sessionId = "replacement-session";
    stamp.generation = 2;
    Check(workflow.Resume(stamp, provider, error), "Explicit recovery did not resume.");
    workflow.Join();
    const auto recovered = workflow.Snapshot();
    const auto& attempts = Node(recovered, "second").attempts;
    Check(attempts.size() == 2 && attempts[0].state == WorkflowState::Failed && attempts[1].verified && recovered.requests == 4 &&
              reviewerCalls == 1 && attempts[1].stamp.sessionId == stamp.sessionId &&
              Node(recovered, "reviewer").attempts[0].prerequisites[1].version == 2,
        "Recovery discarded failure, reset usage or reviewed the wrong artifact version.");
    Check(workflow.Decide(ParentDecision::Rework, error), "Parent rework decision was refused.");
    Check(!workflow.Retry("second", "third input", "third evidence", error), "Recovery exceeded the original attempt budget.");
    Check(!workflow.Start(Spec(), Stamp(), provider, error), "Start reset the budgets of the same workflow.");
}

void TestCancelledLateCompletionIsNotAdmitted()
{
    std::mutex mutex;
    std::condition_variable condition;
    int entered = 0;
    bool release = false;
    AgentWorkflow workflow;
    std::string error;
    Check(workflow.Start(
              Spec(), Stamp(),
              [&](const NodeRequest& request, std::stop_token)
              {
                  std::unique_lock lock(mutex);
                  ++entered;
                  condition.notify_all();
                  while (!release)
                      condition.wait_for(lock, 5ms);
                  return Verified(request);
              },
              error),
        "Cancellation fixture did not start.");
    bool bothEntered;
    {
        std::unique_lock lock(mutex);
        bothEntered = condition.wait_for(lock, 1s, [&] { return entered == 2; });
    }
    workflow.RequestCancel();
    {
        std::lock_guard lock(mutex);
        release = true;
        condition.notify_all();
    }
    workflow.Join();
    const auto snapshot = workflow.Snapshot();
    Check(bothEntered && snapshot.state == WorkflowState::Cancelled && snapshot.requests == 2,
        "Cancellation did not stop dependency dispatch.");
    Check(!workflow.AcceptedArtifact(), "Cancelled work exposed an accepted deliverable.");
    for (const auto& node : snapshot.nodes)
        for (const auto& attempt : node.attempts)
            Check(attempt.executionSucceeded && !attempt.verified && !attempt.artifact && attempt.state == WorkflowState::Cancelled,
                "A late successful cancelled attempt supplied current evidence.");
    workflow.RequestCancel();
    Check(workflow.Snapshot().sequence == snapshot.sequence, "Repeated cancellation emitted redundant observer transitions.");
}

void TestInterruptedCheckpointAndRestartBudgets()
{
    const auto path = Checkpoint("interrupted");
    std::mutex mutex;
    std::condition_variable condition;
    int entered = 0;
    AgentWorkflow original;
    std::string error;
    auto spec = Spec();
    spec.budget.maximumRequests = 4;
    Check(original.Start(
              spec, Stamp(),
              [&](const NodeRequest& request, std::stop_token token)
              {
                  std::unique_lock lock(mutex);
                  ++entered;
                  condition.notify_all();
                  while (!token.stop_requested())
                      condition.wait_for(lock, 5ms);
                  return Verified(request);
              },
              error),
        "Interrupted fixture did not start.");
    bool saved = false;
    {
        std::unique_lock lock(mutex);
        if (condition.wait_for(lock, 1s, [&] { return entered == 2; }))
        {
            lock.unlock();
            saved = original.Save(path, error);
        }
    }
    original.RequestCancel();
    original.Join();
    Check(saved, "An actual in-flight checkpoint was not saved.");
    AgentWorkflow restored;
    Check(restored.Load(path, error), "The actual interrupted checkpoint did not load.");
    const auto loaded = restored.Snapshot();
    Check(loaded.requests == 2 && Node(loaded, "first").attempts.front().state == WorkflowState::Interrupted &&
              loaded.state == WorkflowState::Paused,
        "Load replayed or erased the interrupted attempt.");
    std::atomic<int> calls = 0;
    const auto provider = [&](const NodeRequest& request, std::stop_token)
    {
        ++calls;
        return Verified(request);
    };
    auto wrong = Stamp();
    wrong.companionId = "other-companion";
    Check(!restored.Resume(wrong, provider, error) && calls == 0, "A checkpoint resumed under a different companion.");
    auto resumedStamp = Stamp();
    resumedStamp.sessionId = "resumed-session";
    resumedStamp.generation = 3;
    Check(restored.Resume(resumedStamp, provider, error), "Read-only interrupted work did not explicitly resume.");
    restored.Join();
    const auto complete = restored.Snapshot();
    Check(calls == 2 && complete.requests == 4 && complete.state == WorkflowState::Exhausted &&
              Node(complete, "first").attempts.size() == 2 && Node(complete, "reviewer").attempts.empty() &&
              Node(complete, "first").attempts[0].stamp.sessionId == "fixture-session" &&
              Node(complete, "first").attempts[1].stamp.sessionId == "resumed-session",
        "Restart reset request budgets, original origins or interrupted history.");
    Check(!restored.Decide(ParentDecision::Accept, error), "An exhausted unreviewed workflow was accepted.");
}

void TestDurableEvidenceAndInvalidCheckpointRejection()
{
    const auto path = Checkpoint("completed");
    AgentWorkflow original;
    std::string error;
    Check(original.Start(
              Spec(), Stamp(), [](const NodeRequest& request, std::stop_token) { return Verified(request); }, error),
        "Persistence fixture did not start.");
    original.Join();
    Check(original.EvaluateParent(error), "Persistence fixture parent evaluation was refused.");
    original.Join();
    Check(original.Save(path, error), "Verified evidence was not durably saved.");
    AgentWorkflow restored;
    Check(restored.Load(path, error), "Verified evidence was not loaded.");
    const auto before = restored.Snapshot();
    const auto expected = Node(original.Snapshot(), "reviewer").attempts.front().prerequisites;
    const auto actual = Node(before, "reviewer").attempts.front().prerequisites;
    Check(before.requests == 4 && before.state == WorkflowState::AwaitingAcceptance && actual.size() == expected.size() &&
              actual[0].nodeId == expected[0].nodeId && actual[0].version == expected[0].version && actual[0].hash == expected[0].hash &&
              actual[1].nodeId == expected[1].nodeId && actual[1].version == expected[1].version && actual[1].hash == expected[1].hash,
        "Artifact versions/hashes or spent usage changed on load.");
    Check(restored.Decide(ParentDecision::Accept, error) && restored.Save(path, error), "Separate acceptance did not persist.");
    AgentWorkflow accepted;
    Check(accepted.Load(path, error) && accepted.Snapshot().parentDecision == ParentDecision::Accept,
        "Parent acceptance was lost on restart.");
    nlohmann::json checkpoint;
    {
        std::ifstream stream(path);
        stream >> checkpoint;
    }
    checkpoint["nodes"][0]["readOnly"] = false;
    {
        std::ofstream stream(path);
        stream << checkpoint.dump();
    }
    Check(!restored.Load(path, error) && restored.Snapshot().state == WorkflowState::Accepted,
        "A privileged checkpoint replaced valid owner state.");
    checkpoint["nodes"][0]["readOnly"] = true;
    checkpoint["schema"] = 4;
    {
        std::ofstream stream(path);
        stream << checkpoint.dump();
    }
    Check(!restored.Load(path, error), "A newer checkpoint schema was admitted.");
    checkpoint["schema"] = 2;
    checkpoint["nodes"][1]["attempts"][0]["artifact"]["hash"] = std::string(64, '0');
    {
        std::ofstream stream(path);
        stream << checkpoint.dump();
    }
    Check(!restored.Load(path, error), "Corrupt evidence hash was admitted from disk.");
    const auto directory = Checkpoint("occupied-destination");
    // A run stopped before its cleanup must not break every later one.
    std::filesystem::remove_all(directory);
    std::filesystem::create_directory(directory);
    Check(!original.Save(directory, error) && std::filesystem::is_directory(directory),
        "Failed atomic publication damaged the existing destination.");
    std::filesystem::remove(directory);
}

void TestBoundsPrivacyAndReportedUsage()
{
    std::string error;
    Check(AgentWorkflow::ArtifactHash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "Artifact SHA256 does not match the known digest.");
    auto invalid = Spec();
    invalid.nodes[1].dependsOn = {"reviewer"};
    Check(!AgentWorkflow::Validate(invalid, error), "A cyclic workflow was accepted.");
    invalid = Spec();
    invalid.budget.maximumRequests = 0;
    Check(!AgentWorkflow::Validate(invalid, error), "Zero hard budget was accepted.");
    invalid = Spec();
    invalid.nodes[1].id = invalid.nodes[2].id;
    Check(!AgentWorkflow::Validate(invalid, error), "Duplicate node IDs were accepted.");
    invalid = Spec();
    invalid.nodes[0].readOnly = false;
    Check(!AgentWorkflow::Validate(invalid, error), "An effectful provider spec was accepted.");
    auto spec = Spec();
    spec.nodes[1].inputText = "PRIVATE_SENTINEL";
    spec.nodes[1].objective = "PRIVATE_SENTINEL";
    AgentWorkflow workflow;
    Check(workflow.Start(
              spec, Stamp(),
              [](const NodeRequest& request, std::stop_token)
              {
                  auto result = Verified(request);
                  result.artifact.content = "PRIVATE_SENTINEL";
                  result.artifact.hash = AgentWorkflow::ArtifactHash(result.artifact.content);
                  result.diagnostic = "PRIVATE_SENTINEL";
                  if (request.node.id == "first")
                  {
                      result.reportedTokens = 7;
                      result.reportedModel = "fixture/model";
                  }
                  return result;
              },
              error),
        "Usage fixture did not start.");
    workflow.Join();
    const auto snapshot = workflow.Snapshot();
    Check(snapshot.reportedTokens == 7 && snapshot.unreportedRequests == 2 &&
              Node(snapshot, "first").attempts[0].reportedModel == "fixture/model",
        "Unavailable tokens were estimated or actual reported model/usage was discarded.");
    for (const auto& node : snapshot.nodes)
    {
        Check((node.workingOn + node.currentAction + node.waitingFor + node.nextHandoff).find("PRIVATE_SENTINEL") == std::string::npos,
            "Provider material leaked into presentation.");
        for (const auto& attempt : node.attempts)
            Check(attempt.diagnostic.find("PRIVATE_SENTINEL") == std::string::npos, "Private provider diagnostics leaked into snapshots.");
    }
    AgentWorkflow corrupt;
    Check(corrupt.Start(
              Spec(), Stamp(),
              [](const NodeRequest& request, std::stop_token)
              {
                  auto result = Verified(request);
                  result.artifact.hash = std::string(64, '0');
                  return result;
              },
              error),
        "Corrupt result fixture did not start.");
    corrupt.Join();
    Check(Node(corrupt.Snapshot(), "first").state == WorkflowState::Failed,
        "Provider assertion of verification bypassed content hash validation.");
    workflow.RequestCancel();
    Check(!workflow.Decide(ParentDecision::Accept, error), "Cancelled pending acceptance was accepted.");
}

void TestTimeAndTokenBudgets()
{
    std::string error;
    auto spec = Spec();
    spec.budget.maximumActiveMilliseconds = 30;
    AgentWorkflow timed;
    const auto start = std::chrono::steady_clock::now();
    Check(timed.Start(
              spec, Stamp(),
              [](const NodeRequest& request, std::stop_token token)
              {
                  while (!token.stop_requested())
                      std::this_thread::sleep_for(2ms);
                  return Verified(request);
              },
              error),
        "Timed fixture did not start.");
    timed.Join();
    Check(timed.Snapshot().state == WorkflowState::Exhausted && std::chrono::steady_clock::now() - start < 1s &&
              timed.Snapshot().requests == 2,
        "Measured active budget did not request cooperative cancellation.");
    spec = Spec();
    spec.budget.maximumReportedTokens = 1;
    AgentWorkflow tokenBound;
    Check(tokenBound.Start(
              spec, Stamp(),
              [](const NodeRequest& request, std::stop_token)
              {
                  auto result = Verified(request);
                  result.reportedTokens = 2;
                  return result;
              },
              error),
        "Token fixture did not start.");
    tokenBound.Join();
    Check(tokenBound.Snapshot().state == WorkflowState::Exhausted && Node(tokenBound.Snapshot(), "reviewer").attempts.empty(),
        "A reported usage ceiling did not bound later admission.");
}

void TestParentEvidenceRecoveryAndRestart()
{
    AgentWorkflow workflow;
    std::string error;
    const auto provider = [](const NodeRequest& request, std::stop_token)
    {
        auto result = Verified(request);
        if (request.node.role == WorkflowRole::Parent && request.attempt == 1)
            result.verified = false;
        return result;
    };
    Check(workflow.Start(Spec(), Stamp(), provider, error), "Parent recovery fixture did not start.");
    workflow.Join();
    Check(workflow.EvaluateParent(error), "The first parent evaluation was refused.");
    workflow.Join();
    Check(Node(workflow.Snapshot(), "parent").state == WorkflowState::Failed && !workflow.Decide(ParentDecision::Accept, error),
        "Failed parent evidence was accepted.");
    Check(workflow.Resume(Stamp(), provider, error), "An explicit idle resume was refused.");
    workflow.Join();
    Check(workflow.Snapshot().state == WorkflowState::Paused && !workflow.Decide(ParentDecision::Accept, error),
        "Unchanged resume bypassed failed parent evidence.");
    Check(!workflow.Retry("parent", "", "", error), "Identical failed parent material was retried.");
    // The failed request used empty input/evidence; recovery changes evidence exactly once.
    Check(workflow.Retry("parent", "", "new-parent-evidence", error), "Changed parent evidence recovery was refused.");
    Check(workflow.Resume(Stamp(), provider, error), "Changed parent evidence did not explicitly resume.");
    workflow.Join();
    Check(Node(workflow.Snapshot(), "parent").attempts.size() == 2 && workflow.Snapshot().requests == 5 &&
              Node(workflow.Snapshot(), "parent").attempts.back().verified && workflow.Decide(ParentDecision::Accept, error),
        "The parent recovery did not preserve budgeted failed history and separate acceptance.");
    const auto path = Checkpoint("parent-accepted");
    Check(workflow.Save(path, error), "Real parent acceptance was not saved.");
    AgentWorkflow restored;
    Check(restored.Load(path, error) && restored.Snapshot().state == WorkflowState::Accepted &&
              Node(restored.Snapshot(), "parent").attempts.size() == 2 && restored.Snapshot().requests == 5,
        "Real parent artifact/acceptance did not survive restart.");
    Check(restored.AcceptedArtifact() && restored.AcceptedArtifact()->version == 2, "The accepted deliverable was lost on restart.");
    AgentWorkflow rejected;
    Check(rejected.Start(
              Spec(), Stamp(), [](const NodeRequest& request, std::stop_token) { return Verified(request); }, error),
        "Reject fixture did not start.");
    rejected.Join();
    Check(rejected.EvaluateParent(error), "Reject fixture parent was not evaluated.");
    rejected.Join();
    Check(rejected.Decide(ParentDecision::Reject, error) && !rejected.AcceptedArtifact(), "Rejected parent work exposed a deliverable.");
}

void TestRetryAtVisiblePausedTeardown()
{
    for (int setup = 0; setup < 16; ++setup)
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::thread::id secondThread;
        bool workReleased = false;
        bool firstReleased = false;
        bool dispatchSeen = false;
        bool resultCallbackHeld = false;
        bool resultCallbackReleased = false;
        bool paused = false;
        bool callbackReleased = false;
        bool retryFinished = false;
        bool retryAccepted = false;
        std::string retryError;
        bool callbackRetryRefused = false;
        std::atomic<int> calls = 0;
        AgentWorkflow workflow;
        std::string error;
        Check(workflow.Start(
                  Spec(), Stamp(),
                  [&](const NodeRequest& request, std::stop_token token)
                  {
                      ++calls;
                      std::unique_lock lock(mutex);
                      if (request.node.id == "second")
                          secondThread = std::this_thread::get_id();
                      // "first" finishes only once "second"'s own thread holds the result
                      // drainer, so the dispatcher joins first's thread, and frees its id, before
                      // it blocks on second's. glibc gives a joined thread's id to the next thread
                      // created, here the retry below: that reuse is what this proves harmless.
                      const auto released = [&] { return request.node.id == "first" ? firstReleased : workReleased; };
                      while (!released() && !token.stop_requested())
                          condition.wait_for(lock, 5ms);
                      auto result = Verified(request);
                      if (request.node.id == "second")
                          result.verified = false;
                      return result;
                  },
                  error,
                  [&](const WorkflowSnapshot& snapshot)
                  {
                      const auto running = [&](const char* id)
                      {
                          const auto& node = Node(snapshot, id);
                          return !node.attempts.empty() && node.attempts.back().state == WorkflowState::Running;
                      };
                      if (running("first") && running("second"))
                      {
                          std::lock_guard seen(mutex);
                          dispatchSeen = true;
                          condition.notify_all();
                      }
                      const bool hasCompletedAttempt = std::any_of(snapshot.nodes.begin(), snapshot.nodes.end(),
                          [](const auto& node) { return !node.attempts.empty() && node.attempts.back().state != WorkflowState::Running; });
                      if (snapshot.state == WorkflowState::Running && hasCompletedAttempt)
                      {
                          std::unique_lock lock(mutex);
                          if (!resultCallbackHeld)
                          {
                              // "first" may finish now. Either second's own thread drains its
                              // result and is held here, or another thread did and this setup
                              // cannot qualify, so it should end at once.
                              firstReleased = true;
                              condition.notify_all();
                              if (std::this_thread::get_id() == secondThread)
                              {
                                  resultCallbackHeld = true;
                                  while (!resultCallbackReleased)
                                      condition.wait_for(lock, 5ms);
                              }
                          }
                      }
                      if (snapshot.state != WorkflowState::Paused)
                          return;
                      std::unique_lock lock(mutex);
                      if (paused)
                          return;
                      std::string callbackError;
                      callbackRetryRefused = !workflow.Retry("second", "callback input", "callback evidence", callbackError);
                      paused = true;
                      condition.notify_all();
                      while (!callbackReleased)
                          condition.wait_for(lock, 5ms);
                  }),
            "Visible paused teardown fixture did not start.");
        {
            // Released only once the dispatcher has delivered both dispatched attempts and moved
            // on, so that second's own thread, not the dispatcher, drains its result. The pause
            // grows with each setup, so a setup lost to scheduling is not repeated identically.
            std::unique_lock lock(mutex);
            condition.wait_for(lock, 1s, [&] { return dispatchSeen; });
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(2 + 3 * setup));
            lock.lock();
            workReleased = true;
            condition.notify_all();
        }
        bool qualifyingTeardown = false;
        bool observedPaused = false;
        {
            std::unique_lock lock(mutex);
            condition.wait_for(lock, 1s, [&] { return resultCallbackHeld || paused; });
            firstReleased = true;
            condition.notify_all();
            if (resultCallbackHeld)
            {
                const auto deadline = std::chrono::steady_clock::now() + 1s;
                while (workflow.Snapshot().state != WorkflowState::Paused && std::chrono::steady_clock::now() < deadline)
                    condition.wait_for(lock, 5ms);
                qualifyingTeardown = workflow.Snapshot().state == WorkflowState::Paused;
            }
            resultCallbackReleased = true;
            condition.notify_all();
            observedPaused = condition.wait_for(lock, 1s, [&] { return paused; });
            if (!qualifyingTeardown)
            {
                callbackReleased = true;
                condition.notify_all();
            }
        }
        if (!qualifyingTeardown)
        {
            workflow.Join();
            continue;
        }
        std::jthread retry(
            [&]
            {
                std::string refusal;
                const bool accepted = workflow.Retry("second", "changed input", "changed evidence", refusal);
                std::lock_guard lock(mutex);
                retryAccepted = accepted;
                retryError = refusal;
                retryFinished = true;
                condition.notify_all();
            });
        {
            std::unique_lock lock(mutex);
            condition.wait_for(lock, 30ms, [&] { return retryFinished; });
            callbackReleased = true;
            condition.notify_all();
        }
        retry.join();
        workflow.Join();
        const std::string message =
            "Visible Paused recovery was rejected while the completed dispatcher was tearing down: " + retryError;
        Check(observedPaused && callbackRetryRefused && retryAccepted && calls == 2 &&
                  Node(workflow.Snapshot(), "second").state == WorkflowState::Waiting,
            message.c_str());
        return;
    }
    Check(false, "Fixture could not establish a provider-owned result drainer within its setup bound.");
}
}

void RunAgentWorkflowTests()
{
    TestCheckpointSaveFailureDetails();
    TestCheckpointPendingPathRefusalPreservesPriorCheckpoint();
    TestOversizeCheckpointSaveFailure();
    TestClaimedVerificationRequiresDeliverableSections();
    TestAcceptanceRequiresParentArtifact();
    TestDeliverableBoundsAndExactEvidence();
    TestCompleteDeliverableSurvivesCheckpointValidation();
    TestIndependentWorkersOverlap();
    TestDependencyWaitAndSeparateParentAcceptance();
    TestExpectedFailureAndChangedEvidenceRecovery();
    TestCancelledLateCompletionIsNotAdmitted();
    TestInterruptedCheckpointAndRestartBudgets();
    TestDurableEvidenceAndInvalidCheckpointRejection();
    TestBoundsPrivacyAndReportedUsage();
    TestTimeAndTokenBudgets();
    TestParentEvidenceRecoveryAndRestart();
    TestRetryAtVisiblePausedTeardown();
    std::cout << "Agent workflow tests passed.\n";
}
