#include "Agents/agentWorkflow.h"
#include "Agents/delegation.h"
#include "Agents/toolRegistry.h"
#include "Core/preferenceStore.h"
#include "Policy/companionAuthority.h"
#include "Runtime/runtimeEvents.h"
#include "testSupport.h"
#include "reviaSessionTestAccess.h"
#include "speechServiceTestAccess.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef CreateDirectory
#undef CreateDirectory
#endif

// Chat delegation: Revia handing an investigation to her Agent Studio workers from the
// conversation and reporting what they found. The session checks run a whole chat turn
// against a llama-server stand-in that can hold the workers mid-task, so they can show
// what the person actually experiences: the reply arrives while the work is still running,
// a second message is answered meanwhile, the report comes back in her own reply path, and
// nothing stale is ever shown.
namespace
{
using namespace revia::runtime;
using revia::agents::DelegationCue;
using revia::agents::DelegationMode;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using Json = nlohmann::json;
using namespace std::chrono_literals;

class WorkingDirectory
{
  public:
    explicit WorkingDirectory(const std::filesystem::path& directory) : previous(std::filesystem::current_path())
    {
        std::filesystem::current_path(directory);
    }
    ~WorkingDirectory()
    {
        std::filesystem::current_path(previous);
    }

  private:
    std::filesystem::path previous;
};

struct Subscription
{
    RuntimeEventBus& events;
    RuntimeEventBus::SubscriptionId id;
    ~Subscription()
    {
        events.Unsubscribe(id);
    }
};

Json Text(const std::string& text)
{
    return {{"choices", Json::array({{{"delta", {{"content", text}}}, {"finish_reason", "stop"}}})}};
}

Json Call(const std::size_t index, const std::string& id, const std::string& name, const Json& arguments, const bool last)
{
    Json fragment = {{"index", index}, {"id", id}, {"type", "function"}, {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
    return {{"choices", Json::array({{{"delta", {{"tool_calls", Json::array({fragment})}}}, {"finish_reason", last ? Json("tool_calls") : Json()}}})}};
}

Json ToolMessages(const Json& wire)
{
    Json found = Json::array();
    for (const auto& message : wire.value("messages", Json::array()))
        if (message.value("role", "") == "tool")
            found.push_back(message);
    return found;
}

// The person's newest message; earlier ones stay in the history the model is sent.
std::string LastUserText(const Json& wire)
{
    std::string text;
    for (const auto& message : wire.value("messages", Json::array()))
        if (message.value("role", "") == "user" && message.contains("content") && message["content"].is_string())
            text = message["content"].get<std::string>();
    return text;
}

// Still on its way to an outcome.
bool InFlight(const DelegatedTaskStatus status)
{
    return status == DelegatedTaskStatus::Working || status == DelegatedTaskStatus::Reviewing || status == DelegatedTaskStatus::Reporting;
}

bool Offers(const Json& wire, const std::string& name)
{
    for (const auto& tool : wire.value("tools", Json::array()))
        if (tool.contains("function") && tool["function"].value("name", "") == name)
            return true;
    return false;
}

// A llama-server stand-in that answers by what each request is: a chat step, the report
// turn, a workflow role, a JSON review, or a warmup. Workers can be held until released.
class DelegationBackend
{
  public:
    enum class Kind
    {
        Plain,
        Chat,
        Report,
        JsonObject,
        ToolProposal,
        Worker,
        Reviewer,
        Parent
    };
    using ChatScript = std::function<std::vector<Json>(const Json&)>;

    explicit DelegationBackend(const int slots = 4)
    {
        server.Get("/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
        server.Get("/v1/models",
            [](const auto&, auto& response) { response.set_content(R"({"data":[{"id":"fixture-main"}]})", "application/json"); });
        // Tools need a 16K context. With one slot, as the shipped settings run, every chat
        // request preempts whatever background worker holds the model.
        server.Get("/props", [slots](const auto&, auto& response)
            {
                response.set_content(
                    Json{{"total_slots", slots}, {"default_generation_settings", {{"n_ctx", 32768}}}}.dump(), "application/json");
            });
        server.Post("/v1/chat/completions", [this](const auto& request, auto& response) { Handle(request, response); });
        server.new_task_queue = [] { return new httplib::ThreadPool(12); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "The delegation backend could not bind loopback.");
        worker = std::jthread([this] { server.listen_after_bind(); });
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(1ms);
        Check(server.is_running(), "The delegation backend did not start.");
    }
    ~DelegationBackend()
    {
        ReleaseWorkers();
        ReleaseReports();
        server.stop();
        if (worker.joinable())
            worker.join();
    }

    void SetChat(ChatScript script)
    {
        std::lock_guard lock(mutex);
        chat = std::move(script);
    }
    void SetReport(std::string text)
    {
        std::lock_guard lock(mutex);
        report = std::move(text);
    }
    void SetWorkersVerified(const bool verified)
    {
        std::lock_guard lock(mutex);
        workersVerified = verified;
    }
    // Reported usage on the reviewer's answer, enough to spend the token budget before the
    // parent check can run.
    void SetReviewerTokens(const unsigned tokens)
    {
        std::lock_guard lock(mutex);
        reviewerTokens = tokens;
    }
    // How often the most-asked worker objective reached the model: above one means a
    // worker asked again after yielding.
    unsigned MostRepeatedWorker()
    {
        std::lock_guard lock(mutex);
        std::map<std::string, unsigned> counts;
        unsigned most = 0;
        for (const auto& [seen, wire] : requests)
            if (seen == Kind::Worker)
                most = std::max(most, ++counts[Material(wire).value("objective", std::string{})]);
        return most;
    }
    void HoldWorkers()
    {
        std::lock_guard lock(mutex);
        holding = true;
    }
    void ReleaseWorkers()
    {
        {
            std::lock_guard lock(mutex);
            holding = false;
        }
        changed.notify_all();
    }
    bool WaitForHeldWorkers(const unsigned count, const std::chrono::milliseconds limit)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, limit, [&] { return held >= count; });
    }
    void HoldReports()
    {
        std::lock_guard lock(mutex);
        holdingReports = true;
    }
    void ReleaseReports()
    {
        {
            std::lock_guard lock(mutex);
            holdingReports = false;
        }
        changed.notify_all();
    }
    bool WaitForHeldReports(const unsigned count, const std::chrono::milliseconds limit)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, limit, [&] { return heldReports >= count; });
    }
    std::vector<Json> Requests(const Kind kind)
    {
        std::lock_guard lock(mutex);
        std::vector<Json> matching;
        for (const auto& [seen, wire] : requests)
            if (seen == kind)
                matching.push_back(wire);
        return matching;
    }
    int port = 0;

  private:
    static Json Material(const Json& wire)
    {
        for (const auto& message : wire.value("messages", Json::array()))
        {
            if (!message.contains("content") || !message["content"].is_string())
                continue;
            const auto parsed = Json::parse(message["content"].get<std::string>(), nullptr, false);
            if (parsed.is_object() && parsed.contains("role"))
                return parsed;
        }
        return Json::object();
    }
    static Kind Classify(const Json& wire)
    {
        if (wire.contains("response_format"))
        {
            const auto& format = wire["response_format"];
            if (format.value("type", "") != "json_schema")
                return Kind::JsonObject;
            const auto& properties = format["json_schema"]["schema"].value("properties", Json::object());
            if (properties.contains("tool"))
                return Kind::ToolProposal;
            const auto role = Material(wire).value("role", "");
            return role == "Worker" ? Kind::Worker : role == "Reviewer" ? Kind::Reviewer : Kind::Parent;
        }
        if (!wire.value("stream", false))
            return Kind::Plain;
        return wire.dump().find("tell the person what the agents you delegated to found") != std::string::npos ? Kind::Report : Kind::Chat;
    }
    // A complete deliverable for whatever contract the workflow's schema asks for.
    static Json Deliverable(const Json& wire, const bool verified)
    {
        const auto& properties = wire["response_format"]["json_schema"]["schema"]["properties"];
        Json payload = {{"summary", "The port is set in fixture/service.conf (port=8125)."},
            {"evidence", "Supplied fixture material names fixture/service.conf."}, {"verified", verified},
            {"prerequisiteEvidence", properties["prerequisiteEvidence"].value("const", Json::array())}};
        for (const auto& field : properties.items())
            if (!payload.contains(field.key()))
                payload[field.key()] = {{"items", {"fixture/service.conf sets port=8125."}}, {"noneReason", ""}};
        return payload;
    }
    void Handle(const httplib::Request& request, httplib::Response& response)
    {
        const auto wire = Json::parse(request.body);
        const Kind kind = Classify(wire);
        ChatScript script;
        std::string reportText;
        bool verified = true;
        unsigned tokens = 0;
        {
            std::unique_lock lock(mutex);
            requests.emplace_back(kind, wire);
            if (kind == Kind::Worker && holding)
            {
                ++held;
                changed.notify_all();
                changed.wait_for(lock, 30s, [this] { return !holding; });
                --held;
            }
            if (kind == Kind::Report && holdingReports)
            {
                ++heldReports;
                changed.notify_all();
                changed.wait_for(lock, 30s, [this] { return !holdingReports; });
                --heldReports;
            }
            script = chat;
            reportText = report;
            verified = workersVerified || kind != Kind::Worker;
            tokens = kind == Kind::Reviewer ? reviewerTokens : 0;
        }
        const auto plain = [&response, tokens](const std::string& content)
        {
            Json answer = {{"choices", Json::array({{{"message", {{"content", content}}}, {"finish_reason", "stop"}}})}};
            if (tokens > 0)
                answer["usage"] = {{"prompt_tokens", tokens}, {"completion_tokens", 1}};
            response.set_content(answer.dump(), "application/json");
        };
        const auto stream = [&response](const std::vector<Json>& events)
        {
            std::string body;
            for (const auto& event : events)
                body += "data: " + event.dump() + "\n\n";
            response.set_content(body + "data: [DONE]\n\n", "text/event-stream");
        };
        switch (kind)
        {
        case Kind::Plain:
            return plain("OK");
        case Kind::JsonObject:
            return plain(R"({"verdict":"allow","reason":"Fixture review.","shouldRemember":false})");
        case Kind::ToolProposal:
            return plain(R"({"tool":null})");
        case Kind::Worker:
        case Kind::Reviewer:
        case Kind::Parent:
            return plain(Deliverable(wire, verified).dump());
        case Kind::Report:
            return stream({Text(reportText)});
        case Kind::Chat:
            return stream(script ? script(wire) : std::vector<Json>{Text("Okay.")});
        }
    }

    httplib::Server server;
    std::jthread worker;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<Kind, Json>> requests;
    ChatScript chat;
    std::string report = "My agents traced it: fixture/service.conf sets port 8125, and the review confirmed it.";
    bool workersVerified = true;
    unsigned reviewerTokens = 0;
    bool holding = false;
    unsigned held = 0;
    bool holdingReports = false;
    unsigned heldReports = 0;
};

struct SessionFixture
{
    ScopedTestDirectory temporary;
    WorkingDirectory cwd{temporary.root};
    DelegationBackend backend;
    ReviaSession session;
    std::mutex mutex;
    std::vector<RuntimeEvent> events;
    Subscription subscription;

    explicit SessionFixture(const std::string& id, const int slots = 4)
        : backend(slots), session(CompanionPaths(temporary.root, {id, "Delegation fixture", "assistant", false})),
          subscription{session.Events(), session.Events().Subscribe(
                                             [this](const RuntimeEvent& event)
                                             {
                                                 std::lock_guard lock(mutex);
                                                 events.push_back(event);
                                             })}
    {
        // Nothing serves embeddings here; see NoEmbeddingServer.
        if (slots == 1)
            ReviaSessionTestAccess::ConfigureMainOnly(session, backend.port, ReviaSessionTestAccess::NoEmbeddingServer());
        else
            ReviaSessionTestAccess::ConfigureStartupBrains(session, backend.port, true, true, ReviaSessionTestAccess::NoEmbeddingServer());
        ReviaSessionTestAccess::MarkStudioStarted(session, true);
    }
    ~SessionFixture()
    {
        backend.ReleaseWorkers();
        backend.ReleaseReports();
        ReviaSessionTestAccess::MarkStudioStarted(session, false);
    }

    SessionResult Say(const std::string& text)
    {
        revia::agents::InputBatch batch;
        batch.text = text;
        batch.context = ReviaSessionTestAccess::StudioInput(session, revia::agents::InputSource::Typed);
        return ReviaSessionTestAccess::RunAcceptedTurn(session, batch);
    }
    // The desktop polls every 250 ms; this is the same tick, faster.
    bool PollUntil(const std::function<bool()>& done, const std::chrono::milliseconds limit = 60s)
    {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < until)
        {
            session.PollBackgroundEvents();
            if (done())
                return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }
    // What a failure message needs to explain itself: her record and the workflow's own.
    std::string Describe()
    {
        const auto task = session.DelegatedTask();
        const auto snapshot = session.AgentWorkflowSnapshot();
        std::string text = "status " + (task ? ToString(task->status) + " r" + std::to_string(task->revision) : std::string("none")) +
                           "; workflow " + revia::agents::ToString(snapshot.state) + " (";
        for (const auto& node : snapshot.nodes)
            text += node.id + "=" + revia::agents::ToString(node.state) + "/" + std::to_string(node.attempts.size()) + " ";
        return text + ")" + (task && !task->detail.empty() ? "; " + task->detail : std::string{});
    }
    DelegatedTaskStatus Status()
    {
        const auto task = session.DelegatedTask();
        return task ? task->status : DelegatedTaskStatus::Replaced;
    }
    // Waits until the task leaves Working, Reviewing and Reporting, then says whether it ended as
    // `expected`: a wrong outcome fails at once rather than after the whole wait.
    bool SettlesAs(const DelegatedTaskStatus expected)
    {
        (void)PollUntil(
            [&]
            {
                const auto task = session.DelegatedTask();
                return !task || !InFlight(task->status);
            });
        return Status() == expected;
    }
    // The same for an outcome whose report is held back: it must stop at Reporting.
    bool HeldAtReporting()
    {
        (void)PollUntil(
            [&]
            {
                const auto task = session.DelegatedTask();
                return !task || (task->status != DelegatedTaskStatus::Working && task->status != DelegatedTaskStatus::Reviewing);
            });
        return Status() == DelegatedTaskStatus::Reporting;
    }
    std::vector<RuntimeEvent> Reports()
    {
        std::lock_guard lock(mutex);
        std::vector<RuntimeEvent> found;
        for (const auto& event : events)
            if (event.kind == RuntimeEventKind::AssistantMessage && event.component == "Delegation")
                found.push_back(event);
        return found;
    }
    std::vector<RuntimeEvent> Snapshot()
    {
        std::lock_guard lock(mutex);
        return events;
    }
};

// ---- Deterministic pieces -----------------------------------------------------------

void TestTheCueComesFromThePersonsOwnWords()
{
    using revia::agents::ReadDelegationCue;
    Check(ReadDelegationCue("Check why your audio keeps cutting out, use your agents to verify the cause.") == DelegationCue::UseAgents,
        "An explicit request to use the agents was not read.");
    Check(ReadDelegationCue("Have your workers look into the crash log") == DelegationCue::UseAgents &&
              ReadDelegationCue("Can you delegate this to Agent Studio?") == DelegationCue::UseAgents,
        "Other ways of asking for the agents were not read.");
    Check(ReadDelegationCue("Don't use your agents, just tell me.") == DelegationCue::AnswerDirectly &&
              ReadDelegationCue("Answer this directly please") == DelegationCue::AnswerDirectly &&
              ReadDelegationCue("don’t delegate it") == DelegationCue::AnswerDirectly,
        "Asking for a direct answer did not win over the words it contains.");
    Check(ReadDelegationCue("What do you think of that character?") == DelegationCue::None &&
              ReadDelegationCue("What's in the background of this picture?") == DelegationCue::None &&
              ReadDelegationCue("Who directed this movie?") == DelegationCue::None,
        "Ordinary conversation was read as a delegation cue.");
}

void TestOfferingFollowsModeAndWords()
{
    using revia::agents::OffersDelegation;
    Check(!OffersDelegation(DelegationMode::Off, DelegationCue::UseAgents), "Off offered delegation.");
    Check(!OffersDelegation(DelegationMode::OnRequest, DelegationCue::None) &&
              OffersDelegation(DelegationMode::OnRequest, DelegationCue::UseAgents),
        "On request did not wait for the person to ask.");
    Check(OffersDelegation(DelegationMode::Auto, DelegationCue::None) && !OffersDelegation(DelegationMode::Auto, DelegationCue::AnswerDirectly),
        "Auto did not leave the choice to the model, or ignored \"answer directly\".");
    Check(revia::agents::DelegationModeFromString("auto") == DelegationMode::Auto &&
              revia::agents::DelegationModeFromString("on_request") == DelegationMode::OnRequest &&
              revia::agents::DelegationModeFromString("Auto") == DelegationMode::Off &&
              revia::agents::DelegationModeFromString("") == DelegationMode::Off,
        "An inexact mode spelling widened delegation.");
}

void TestRegistryOffersDelegationOnlyWhenAllowed()
{
    using namespace revia::agents;
    ToolExecutors executors;
    std::vector<DelegationSubmission> submitted;
    executors.delegateTask = [&submitted](const DelegationSubmission& submission, const ToolCallContext&)
    {
        submitted.push_back(submission);
        return ToolExecution::Ok(R"({"task_id":"t-1","status":"submitted"})");
    };
    executors.taskStatus = [](const std::string&, const ToolCallContext&) { return ToolExecution::Ok("{}"); };
    executors.cancelTask = [](const std::string&, const ToolCallContext&) { return ToolExecution::Ok("Cancelled."); };
    ToolPermissions permissions;
    permissions.privateAudience = true;
    const auto offered = [&](const std::string& name) { return ToolRegistry::Project(permissions, executors).Find(name) != nullptr; };

    Check(!offered("delegate_task") && !offered("task_status") && !offered("cancel_task"), "Off offered delegation tools.");
    permissions.delegationMode = DelegationMode::OnRequest;
    Check(!offered("delegate_task"), "On request offered delegation before the person asked.");
    permissions.delegationCue = DelegationCue::UseAgents;
    Check(offered("delegate_task"), "On request did not offer delegation when asked.");
    permissions.delegationMode = DelegationMode::Auto;
    permissions.delegationCue = DelegationCue::AnswerDirectly;
    Check(!offered("delegate_task"), "\"Answer directly\" still offered delegation.");
    permissions.delegationCue = DelegationCue::None;
    Check(offered("delegate_task"), "Auto did not offer delegation.");

    // A running task stays reachable however the mode was changed after it started.
    permissions.delegationMode = DelegationMode::Off;
    permissions.delegatedTaskKnown = true;
    Check(offered("task_status") && offered("cancel_task") && !offered("delegate_task"),
        "A known task was stranded without status and cancel, or Off still offered new delegation.");
    permissions.privateAudience = false;
    Check(ToolRegistry::Project(permissions, executors).Empty(), "A public audience was offered delegation tools.");

    permissions.privateAudience = true;
    permissions.delegationMode = DelegationMode::Auto;
    const auto registry = ToolRegistry::Project(permissions, executors);
    const ChatTool* tool = registry.Find("delegate_task");
    Json arguments;
    std::string error;
    Check(tool && tool->effect == ToolEffect::Effect && !tool->parallelSafe, "delegate_task may overlap or is marked read-only.");
    Check(!ValidateToolArguments(tool->definition.parametersSchema, Json{{"objective", std::string(1001, 'x')}, {"reason", "r"}}.dump(),
              arguments, error) &&
              !ValidateToolArguments(tool->definition.parametersSchema, R"({"objective":"Check the log."})", arguments, error) &&
              ValidateToolArguments(
                  tool->definition.parametersSchema, R"({"objective":"Check the log.","reason":"Needs an independent check."})", arguments, error),
        "The delegate_task schema did not bound its objective or require a reason: " + error);
    Check(tool->definition.description.find("nothing has been checked yet") != std::string::npos &&
              tool->definition.description.find("cannot change anything") != std::string::npos,
        "The tool's description no longer says that it returns before any work and that workers are read-only.");
    const auto ran = tool->execute(arguments, ToolCallContext{});
    Check(ran.succeeded && submitted.size() == 1 && submitted[0].objective == "Check the log." &&
              submitted[0].reason == "Needs an independent check." && submitted[0].userRequest.empty(),
        "The registry did not hand the model's words, and only those, to the executor.");
}

void TestDelegationSettingIsValidatedAndLive()
{
    ScopedTestDirectory directory;
    revia::core::PreferenceStore store(directory.root / "preferences.json");
    Check(store.Set("conversation.delegation", "auto").succeeded, "The delegation preference refused a valid mode.");
    Check(!store.Set("conversation.delegation", "Auto").succeeded && !store.Set("conversation.delegation", "always").succeeded,
        "The delegation preference accepted an unknown mode.");
    // Pacing, not permission: it decides whether she may reach for her workers, never what
    // they may touch.
    Check(!revia::core::PreferenceStore::IsAuthoritySetting("conversation.delegation"),
        "The delegation preference reads as an authority setting.");
    appSettings settings;
    Check(settings.conversation.delegation == "on_request", "Delegation is not on request by default.");
    store.Apply(settings);
    Check(settings.conversation.delegation == "auto", "The saved delegation preference was not applied.");
}

// The candidate is exactly the parent's verified deliverable, and acceptance binds to it.
void TestWorkflowAcceptsOnlyTheExaminedCandidate()
{
    using namespace revia::agents;
    if (AgentWorkflow::ArtifactHash("probe").empty())
    {
        std::cout << "Workflow candidate checks skipped: this platform build has no artifact hash.\n";
        return;
    }
    WorkflowSpec spec;
    spec.id = "candidate-fixture";
    const DeliverableRequirement steps{DeliverableSection::Steps, "List what was found.", false};
    spec.nodes = {{"parent", "", WorkflowRole::Parent, 3, "Acceptance", "Accept", "objective", "e1", {"review"}, true, {{steps}, true}},
        {"first", "parent", WorkflowRole::Worker, 1, "First", "Investigate", "objective", "e1", {}, true, {{steps}, false}},
        {"second", "parent", WorkflowRole::Worker, 1, "Second", "Check", "objective", "e1", {}, true, {{steps}, false}},
        {"review", "parent", WorkflowRole::Reviewer, 1, "Reviewer", "Review", "objective", "e1", {"first", "second"}, true, {{steps}, true}}};
    RuntimeStamp stamp;
    stamp.companionId = "fixture";
    stamp.sessionId = "session";
    stamp.generation = 1;
    stamp.taskId = spec.id;
    AgentWorkflow workflow;
    const auto provider = [](const NodeRequest& request, std::stop_token)
    {
        NodeResult result;
        Json payload = {{"summary", "Found."}, {"evidence", "Fixture."}, {"steps", {{"items", {"Found it."}}, {"noneReason", ""}}}};
        Json references = Json::array();
        for (const auto& reference : request.prerequisiteReferences)
            references.push_back({{"nodeId", reference.nodeId}, {"id", reference.id}, {"version", reference.version}, {"hash", reference.hash}});
        payload["prerequisiteEvidence"] = references;
        result.succeeded = result.verified = true;
        result.artifact.content = payload.dump();
        result.artifact.id = request.node.id + "-artifact";
        result.artifact.version = request.attempt;
        result.artifact.hash = AgentWorkflow::ArtifactHash(result.artifact.content);
        return result;
    };
    std::string error;
    Check(workflow.Start(spec, stamp, provider, error), error);
    workflow.Join();
    Check(workflow.Snapshot().state == WorkflowState::AwaitingAcceptance && !workflow.CandidateArtifact(),
        "A candidate existed before the parent evaluation ran.");
    Check(workflow.EvaluateParent(error), error);
    workflow.Join();
    const auto candidate = workflow.CandidateArtifact();
    Check(candidate.has_value() && candidate->id == "parent-artifact", "The evaluated parent deliverable is not the candidate.");
    ArtifactReference stale{"parent", candidate->id, candidate->version, std::string(64, '0')};
    Check(!workflow.AcceptCandidate(stale, error) && workflow.Snapshot().state == WorkflowState::AwaitingAcceptance,
        "A deliverable nobody examined was accepted.");
    Check(workflow.AcceptCandidate({"parent", candidate->id, candidate->version, candidate->hash}, error) &&
              workflow.Snapshot().state == WorkflowState::Accepted && workflow.AcceptedArtifact()->hash == candidate->hash,
        "The examined deliverable was not the one accepted: " + error);
    Check(!workflow.CandidateArtifact(), "An accepted deliverable is still offered as a candidate.");
}

// Narrowing worker tasks leave the shared revision alone, so the conversation beside them
// keeps its admission; everything else still advances it, and an ended task stays refused.
void TestNarrowingTasksDoNotRevokeOtherWork()
{
    revia::policy::CompanionAuthority authority;
    RuntimeStamp session;
    session.companionId = "fixture";
    session.sessionId = "session";
    session.generation = 1;
    authority.SetCompanionDefaults("fixture", revia::policy::AuthorityPermissions::WithinMachineCeiling());
    Check(authority.RegisterSession(session), "The fixture session was not registered.");
    auto worker = session;
    worker.taskId = "worker-attempt";
    const auto before = authority.Revision();
    Check(authority.RegisterTask(worker, {}, revia::policy::AuthorityPermissions::WithinMachineCeiling(), false) &&
              authority.Revision() == before && authority.IsActive(worker),
        "Registering a narrowing task advanced the revision or did not activate it.");
    authority.EndTask(worker);
    Check(authority.Revision() == before && !authority.IsActive(worker), "Ending a narrowing task advanced the revision or left it active.");
    auto ordinary = session;
    ordinary.taskId = "ordinary-task";
    Check(authority.RegisterTask(ordinary, {}) && authority.Revision() > before, "An ordinary task no longer advances the revision.");
    auto child = session;
    child.taskId = "advancing-child";
    auto parent = session;
    parent.taskId = "narrow-parent";
    Check(authority.RegisterTask(parent, {}, revia::policy::AuthorityPermissions::WithinMachineCeiling(), false) &&
              authority.RegisterTask(child, parent.taskId),
        "The nested fixture tasks were not registered.");
    const auto nested = authority.Revision();
    authority.EndTask(parent);
    Check(authority.Revision() > nested && !authority.IsActive(child), "Ending a subtree with an ordinary task did not advance the revision.");
}

// ---- Whole session ----------------------------------------------------------------

// The core experience: she hands the work off, answers at once, talks while it runs, and
// reports the reviewed result in the same conversation; only then is it accepted.
void TestChatDelegationReportsThroughHerReply()
{
    SessionFixture fixture("delegation-report");
    fixture.backend.HoldWorkers();
    std::atomic<unsigned> calls{0};
    fixture.backend.SetChat(
        [&calls](const Json& wire)
        {
            if (ToolMessages(wire).empty() && Offers(wire, "delegate_task"))
            {
                // Asking twice in one step and once more later must still start one task.
                const Json arguments = {{"objective", "Find which fixture file sets the service port and verify it."},
                    {"reason", "It needs a separate check of the files."}};
                calls += 2;
                return std::vector<Json>{Call(0, "d1", "delegate_task", arguments, false), Call(1, "d2", "delegate_task", arguments, true)};
            }
            if (ToolMessages(wire).size() == 2 && Offers(wire, "delegate_task"))
            {
                ++calls;
                return std::vector<Json>{Call(0, "d3", "delegate_task",
                    {{"objective", "Find which fixture file sets the service port and verify it."}, {"reason", "Again."}}, true)};
            }
            return std::vector<Json>{Text(ToolMessages(wire).empty() ? "Still on it; the agents are checking."
                                                                      : "Okay, my agents are checking which file sets the port.")};
        });
    const auto acknowledged = fixture.Say("Use your agents to check which fixture file sets the service port.");
    Check(acknowledged.succeeded && acknowledged.text.find("my agents are checking") != std::string::npos,
        "The delegating turn did not answer at once: " + acknowledged.reason + " / " + acknowledged.text);
    Check(fixture.backend.WaitForHeldWorkers(2, 10s), "The workers did not start in the background.");
    const auto task = fixture.session.DelegatedTask();
    Check(task && task->status == DelegatedTaskStatus::Working && task->originTurnId != 0 &&
              fixture.session.AgentWorkflowSnapshot().id == task->taskId &&
              fixture.session.AgentWorkflowSnapshot().state == revia::agents::WorkflowState::Running,
        "The reply waited for the work, or the task lost its identity or origin.");
    Check(task->access.find("analysis only") != std::string::npos, "A session without approved folders was given tool access.");
    const auto chat = fixture.backend.Requests(DelegationBackend::Kind::Chat);
    Check(calls.load() == 3 && chat.size() == 3, "The fixture did not ask for delegation three times.");
    const auto second = ToolMessages(chat[1]);
    const auto third = ToolMessages(chat[2]);
    Check(second.size() == 2 && second[0].value("content", "").find(task->taskId) != std::string::npos &&
              second[1].value("content", "").find("already_submitted") != std::string::npos &&
              third.back().value("content", "").find("already_submitted") != std::string::npos,
        "Repeated delegate_task calls on one turn were not answered with the task already started.");

    // She can be talked to while the work runs, and can tell how it is going.
    const auto meanwhile = fixture.Say("How is it going?");
    const auto lastChat = fixture.backend.Requests(DelegationBackend::Kind::Chat).back();
    Check(meanwhile.succeeded && meanwhile.text.find("Still on it") != std::string::npos,
        "A message during the background work was not answered: " + meanwhile.reason);
    Check(Offers(lastChat, "task_status") && Offers(lastChat, "cancel_task") && !Offers(lastChat, "delegate_task"),
        "During the task, status and cancel were not offered, or new delegation was offered without being asked.");
    Check(fixture.Reports().empty() && fixture.session.AgentWorkflowSnapshot().state == revia::agents::WorkflowState::Running,
        "Something was reported, or the work was disturbed, before the workers finished.");

    fixture.backend.ReleaseWorkers();
    const bool settled = fixture.SettlesAs(DelegatedTaskStatus::Reported);
    Check(settled, "The reviewed work was never reported; " + fixture.Describe());
    const auto reports = fixture.Reports();
    Check(reports.size() == 1 && reports[0].phase == task->taskId && reports[0].audienceRevision != 0 &&
              reports[0].message.find("fixture/service.conf") != std::string::npos,
        "The report did not arrive once, in her own reply, for this task.");
    const auto snapshot = fixture.session.AgentWorkflowSnapshot();
    const auto finished = fixture.session.DelegatedTask();
    Check(snapshot.state == revia::agents::WorkflowState::Accepted && finished->reportedArtifact && !snapshot.nodes.empty() &&
              snapshot.nodes.front().role == revia::agents::WorkflowRole::Parent && snapshot.nodes.front().attempts.back().artifact &&
              finished->reportedArtifact->hash == snapshot.nodes.front().attempts.back().artifact->hash,
        "The workflow was not accepted, or not with the deliverable she reported.");
    const auto reportRequests = fixture.backend.Requests(DelegationBackend::Kind::Report);
    Check(reportRequests.size() == 1 && !reportRequests[0].contains("tools") &&
              reportRequests[0].dump().find(finished->reportedArtifact->hash) != std::string::npos &&
              reportRequests[0].dump().find("Use your agents to check which fixture file sets the service port") != std::string::npos,
        "The report turn had tools, or did not examine the exact deliverable and the person's own words.");
    // Accepted only after the report: the acceptance snapshot is published after the message.
    const auto events = fixture.Snapshot();
    const auto reported = std::find_if(events.begin(), events.end(),
        [](const auto& event) { return event.kind == RuntimeEventKind::AssistantMessage && event.component == "Delegation"; });
    const auto accepted = std::find_if(events.begin(), events.end(),
        [](const auto& event) { return event.kind == RuntimeEventKind::AgentWorkflow && event.component == "Agent Studio" && event.phase == "Accepted"; });
    Check(reported != events.end() && accepted != events.end() && reported < accepted, "The work was accepted before she reported it.");
    // Saved just after the status changes, so read once the file itself says so.
    Json persisted;
    (void)fixture.PollUntil(
        [&]
        {
            std::ifstream record(fixture.session.Paths().Resolve("RuntimeData/Agents/delegation.json"));
            persisted = Json::parse(record, nullptr, false);
            return persisted.is_object() && persisted.value("status", "") == "Reported";
        },
        10s);
    Check(persisted.is_object() && persisted.value("taskId", "") == task->taskId && persisted.value("status", "") == "Reported" &&
              persisted.value("originTurnId", 0ULL) == task->originTurnId && persisted.contains("budget"),
        "The task's identity, origin, budget and outcome were not recorded: " + persisted.dump());
}

// Cancelling names the task: another id is refused, and a cancelled task never reports.
void TestCancelReachesOnlyTheNamedTask()
{
    SessionFixture fixture("delegation-cancel");
    fixture.backend.HoldWorkers();
    fixture.backend.SetChat(
        [&fixture](const Json& wire)
        {
            const auto results = ToolMessages(wire);
            if (results.empty() && Offers(wire, "delegate_task"))
                return std::vector<Json>{Call(0, "d", "delegate_task", {{"objective", "Check the fixture log."}, {"reason", "Separate check."}}, true)};
            if (results.empty() && Offers(wire, "cancel_task"))
            {
                const auto task = fixture.session.DelegatedTask();
                const std::string asked =
                    LastUserText(wire).find("the other one") != std::string::npos ? std::string("not-this-task") : task->taskId;
                return std::vector<Json>{Call(0, "c", "cancel_task", {{"task_id", asked}}, true)};
            }
            return std::vector<Json>{Text("Done.")};
        });
    Check(fixture.Say("Use your agents to check the fixture log.").succeeded, "The delegating turn failed.");
    Check(fixture.backend.WaitForHeldWorkers(2, 10s), "The workers did not start.");
    const auto task = fixture.session.DelegatedTask();
    std::string error;
    Check(!fixture.session.CancelDelegatedTask("not-this-task", error) && fixture.Status() == DelegatedTaskStatus::Working,
        "Another id cancelled the task.");
    Check(fixture.Say("Cancel the other one.").succeeded && fixture.Status() == DelegatedTaskStatus::Working &&
              ToolMessages(fixture.backend.Requests(DelegationBackend::Kind::Chat).back())
                      .back()
                      .value("content", "")
                      .find("No delegated task has that id") != std::string::npos,
        "cancel_task with a different id stopped this task or did not say why it refused.");
    Check(fixture.Say("Cancel that investigation.").succeeded && fixture.Status() == DelegatedTaskStatus::Cancelled &&
              fixture.session.AgentWorkflowSnapshot().state == revia::agents::WorkflowState::Cancelled,
        "cancel_task with the task's id did not cancel it.");
    fixture.backend.ReleaseWorkers();
    (void)fixture.PollUntil([] { return false; }, 1500ms);
    Check(fixture.Reports().empty() && fixture.Status() == DelegatedTaskStatus::Cancelled && fixture.session.DelegatedTask()->taskId == task->taskId,
        "A cancelled task reported or changed afterwards.");
}

// An outcome waiting for the conversation to be free is never shown once the task is
// cancelled or another workflow has taken the workers.
void TestStaleOutcomesAreNeverReported()
{
    for (const bool replace : {false, true})
    {
        SessionFixture fixture(replace ? "delegation-replaced" : "delegation-stale");
        fixture.backend.SetChat(
            [](const Json& wire)
            {
                if (ToolMessages(wire).empty() && Offers(wire, "delegate_task"))
                    return std::vector<Json>{
                        Call(0, "d", "delegate_task", {{"objective", "Check the fixture port."}, {"reason", "Separate check."}}, true)};
                return std::vector<Json>{Text("Handed off.")};
            });
        Check(fixture.Say("Use your agents to check the fixture port.").succeeded, "The delegating turn failed.");
        // Her conversation is busy, so the reviewed outcome has to wait for it.
        auto foreground = ReviaSessionTestAccess::HoldForeground(fixture.session);
        Check(fixture.HeldAtReporting(), "The outcome never became ready; " + fixture.Describe());
        std::string error;
        // Its parent evaluation may still be tearing down; Agent Studio starts only once idle.
        ReviaSessionTestAccess::JoinWorkflow(fixture.session);
        if (replace)
        {
            Check(fixture.session.StartAgentWorkflow("A different Agent Studio objective.", AgentProviderMode::Demonstration, error), error);
            Check(fixture.Status() == DelegatedTaskStatus::Replaced, "Starting another workflow did not retire the chat task.");
        }
        else
        {
            Check(fixture.session.CancelDelegatedTask(fixture.session.DelegatedTask()->taskId, error), error);
        }
        foreground.unlock();
        (void)fixture.PollUntil([] { return false; }, 2s);
        Check(fixture.Reports().empty() && fixture.backend.Requests(DelegationBackend::Kind::Report).empty(),
            replace ? "A replaced task's outcome was reported." : "A cancelled task's outcome was reported.");
        Check(replace || fixture.session.AgentWorkflowSnapshot().state != revia::agents::WorkflowState::Accepted,
            "A cancelled outcome was accepted.");
    }
}

// Work the review could not verify is reported as not established, and never accepted.
void TestUnverifiedWorkIsReportedAsNotEstablished()
{
    SessionFixture fixture("delegation-unverified");
    fixture.backend.SetWorkersVerified(false);
    fixture.backend.SetReport("My agents could not establish which file sets the port; nothing they found was verified.");
    fixture.backend.SetChat(
        [](const Json& wire)
        {
            if (ToolMessages(wire).empty() && Offers(wire, "delegate_task"))
                return std::vector<Json>{Call(0, "d", "delegate_task", {{"objective", "Check the fixture port."}, {"reason", "Separate check."}}, true)};
            return std::vector<Json>{Text("Handed off.")};
        });
    Check(fixture.Say("Use your agents to check the fixture port.").succeeded, "The delegating turn failed.");
    const bool settled = fixture.SettlesAs(DelegatedTaskStatus::NotEstablished);
    Check(settled, "Unverified work was not reported as not established; " + fixture.Describe());
    const auto report = fixture.backend.Requests(DelegationBackend::Kind::Report);
    Check(fixture.Reports().size() == 1 && report.size() == 1 && report[0].dump().find("did NOT verify") != std::string::npos &&
              fixture.session.AgentWorkflowSnapshot().state != revia::agents::WorkflowState::Accepted,
        "Unverified work was presented as verified, or accepted.");
}

// With approved folders the workers get tools, but only to read; and starting them from chat
// no longer revokes the reply that announces them.
void TestToolWorkersAreReadOnlyAndKeepTheReply()
{
    SessionFixture fixture("delegation-tools");
    const auto approved = fixture.temporary.root / "approved";
    std::filesystem::create_directories(approved);
    std::ofstream(approved / "service.conf") << "port=8125\n";
    {
        std::ofstream output(fixture.temporary.root / "capabilities.json");
        output << Json{{"mode", "approved_scope"}, {"approvedRoots", {revia::actions::PathToUtf8(approved)}},
            {"autoApproveRiskThrough", "reversible_write"}, {"createMissingApprovedRoots", false}}
                      .dump();
    }
    std::string error;
    Check(ReviaSessionTestAccess::Actions(fixture.session)
              .Initialize(fixture.temporary.root / "capabilities.json", fixture.temporary.root / "action-audit.jsonl", error),
        error);
    // The owner also allows a process and command interpreters; delegation must take neither.
    const auto tool = approved / "fixture-tool.exe";
    std::ofstream(tool) << "fixture";
    revia::process::ProcessSettings process;
    process.enabled = process.allowTaskExecution = process.allowCommandInterpreters = true;
    process.approvedExecutables = {tool};
    process.maxTimeoutMs = 1000;
    process.maxOutputBytes = 512;
    Check(ReviaSessionTestAccess::Actions(fixture.session).SetProcessSettings(process, error), error);
    fixture.backend.HoldWorkers();
    fixture.backend.SetChat(
        [](const Json& wire)
        {
            if (ToolMessages(wire).empty() && Offers(wire, "delegate_task"))
                return std::vector<Json>{Call(0, "d", "delegate_task", {{"objective", "Find the service port."}, {"reason", "Separate check."}}, true)};
            return std::vector<Json>{Text("Okay, my agents are reading the config.")};
        });
    const auto reply = fixture.Say("Use your agents to find the service port.");
    Check(reply.succeeded && reply.text.find("my agents are reading") != std::string::npos,
        "Starting tool workers revoked the reply that announced them: " + reply.reason);
    const auto task = fixture.session.DelegatedTask();
    Check(task && task->access.find("read-only") != std::string::npos, "Approved folders did not give the workers read access.");
    std::ifstream input(fixture.session.Paths().Resolve("RuntimeData/Agents/tool-scope.json"));
    const auto scope = Json::parse(input, nullptr, false);
    Check(scope.is_object() && scope.value("workflowId", "") == task->taskId && scope.value("writes", true) == false &&
              scope.value("process", true) == false && scope.value("commandInterpreters", true) == false &&
              scope.value("executables", Json::array({1})).empty(),
        "Delegation captured more than reading, although the owner allows writes: " + scope.dump());
    fixture.backend.ReleaseWorkers();
    Check(fixture.PollUntil([&] { return !fixture.backend.Requests(DelegationBackend::Kind::ToolProposal).empty(); }, 20s),
        "The tool workers never proposed a tool.");
    for (const auto& proposal : fixture.backend.Requests(DelegationBackend::Kind::ToolProposal))
    {
        const auto schema = proposal["response_format"]["json_schema"]["schema"].dump();
        Check(schema.find("write_text_file") == std::string::npos && schema.find("execute_process") == std::string::npos,
            "A delegated worker was offered a write or a process.");
    }
    const bool settled = fixture.SettlesAs(DelegatedTaskStatus::Reported);
    Check(settled, "The read-only investigation was not reported; " + fixture.Describe());
}

// A goal task only narrows what the session may do, so a goal starting or finishing while
// she answers must not revoke the reply. Its own and its background task's stamps are still
// refused once it ends, and a real authority change in the middle of a reply still revokes it.
void TestGoalTasksKeepTheReply()
{
    // Shared with the goal's worker thread, so declared before the session that joins it.
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> launched{false};
    std::mutex stampMutex;
    RuntimeStamp goalStamp;
    std::string launchMessage;
    SessionFixture fixture("delegation-goal-reply");
    auto& session = fixture.session;
    // However a check below ends, the goal is let go and finished before anything it uses.
    struct FinishGoal
    {
        ReviaSession& session;
        std::atomic<bool>& release;
        ~FinishGoal()
        {
            release.store(true);
            ReviaSessionTestAccess::WaitForTask(session);
        }
    } finishGoal{session, release};
    const auto heldGoal = [&](const std::stop_token stop)
    {
        // The real goal scope: it registers the goal's own task under the background task
        // LaunchTask registered, and ends it on the way out.
        ReviaSessionTestAccess::WithGoalAdmission(session,
            [&](const RuntimeStamp& stamp, bool)
            {
                {
                    std::lock_guard lock(stampMutex);
                    goalStamp = stamp;
                }
                entered.store(true);
                while (!release.load() && !stop.stop_requested())
                    std::this_thread::sleep_for(2ms);
            });
        revia::goals::Goal goal;
        goal.title = "goal reply fixture";
        goal.status = revia::goals::GoalStatus::Succeeded;
        return goal;
    };

    // A goal starts while she is answering and is still running when the reply is delivered.
    fixture.backend.SetChat(
        [&](const Json&)
        {
            if (!launched.exchange(true) && ReviaSessionTestAccess::LaunchTask(session, "goal reply fixture", heldGoal, launchMessage))
            {
                const auto until = std::chrono::steady_clock::now() + 10s;
                while (!entered.load() && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(2ms);
            }
            return std::vector<Json>{Text("Here is my answer while the goal starts.")};
        });
    const auto started = fixture.Say("Tell me about the weather while that goal starts.");
    Check(entered.load() && session.HasRunningTask(), "The goal fixture did not start during the reply: " + launchMessage);
    Check(started.succeeded && started.text.find("while the goal starts") != std::string::npos,
        "A goal starting mid-reply revoked the reply: " + started.reason);

    // The same goal finishes, its own task and then the background task, during the next reply.
    fixture.backend.SetChat(
        [&](const Json&)
        {
            release.store(true);
            ReviaSessionTestAccess::WaitForTask(session);
            return std::vector<Json>{Text("Here is my answer while the goal ends.")};
        });
    const auto ended = fixture.Say("And now while it finishes.");
    RuntimeStamp finishedGoal;
    {
        std::lock_guard lock(stampMutex);
        finishedGoal = goalStamp;
    }
    std::optional<RuntimeStamp> taskOrigin;
    {
        std::lock_guard lock(fixture.mutex);
        for (const auto& event : fixture.events)
            if (event.kind == RuntimeEventKind::AssistantMessage && event.component == "Task")
                taskOrigin = event.stamp;
    }
    Check(!session.HasRunningTask() && !finishedGoal.taskId.empty() && !session.Authority()->IsActive(finishedGoal) && taskOrigin &&
              !taskOrigin->taskId.empty() && !session.Authority()->IsActive(*taskOrigin),
        "The goal did not end, or its own or its background task's authority outlived it.");
    // Nothing else changed, so the report's stamp is still current: the owner can give feedback
    // on it, which its own registration used to make impossible.
    Check(taskOrigin->policyVersion == session.Authority()->Revision(), "The task report's stamp went stale by itself.");
    Check(ended.succeeded && ended.text.find("while the goal ends") != std::string::npos,
        "A goal finishing mid-reply revoked the reply: " + ended.reason);

    // Control: a real authority change in the middle of a reply still revokes it.
    fixture.backend.SetChat(
        [&](const Json&)
        {
            session.Authority()->SetCompanionDefaults(
                session.Stamp().companionId, revia::policy::AuthorityPermissions::WithinMachineCeiling());
            return std::vector<Json>{Text("This reply must not arrive.")};
        });
    const auto revoked = fixture.Say("One more question.");
    Check(!revoked.succeeded && revoked.text.find("must not arrive") == std::string::npos,
        "A real authority change no longer revokes the reply in flight.");
}

// On one model slot her own replies preempt the workers. They yield, wait for her turn to
// finish and try again, instead of failing the task the moment she speaks.
void TestWorkersYieldToTheConversation()
{
    SessionFixture fixture("delegation-yield", 1);
    fixture.backend.HoldWorkers();
    fixture.backend.SetChat(
        [](const Json& wire)
        {
            if (ToolMessages(wire).empty() && Offers(wire, "delegate_task"))
                return std::vector<Json>{Call(0, "d", "delegate_task", {{"objective", "Check the fixture port."}, {"reason", "Separate check."}}, true)};
            return std::vector<Json>{Text(ToolMessages(wire).empty() ? "Still checking." : "Okay, handing that to my agents.")};
        });
    Check(fixture.Say("Use your agents to check the fixture port.").succeeded, "The delegating turn failed on one slot.");
    Check(fixture.backend.WaitForHeldWorkers(1, 10s), "No worker reached the model.");
    Check(fixture.Say("How is it going?").succeeded && fixture.Status() == DelegatedTaskStatus::Working,
        "Talking during the work failed, or ended it.");
    fixture.backend.ReleaseWorkers();
    const bool settled = fixture.SettlesAs(DelegatedTaskStatus::Reported);
    Check(settled, "Workers preempted by the conversation did not resume; " + fixture.Describe());
    Check(fixture.backend.MostRepeatedWorker() >= 2, "No worker asked again after yielding to the conversation.");
}

// A run that spends its budget before the acceptance check can never be accepted; it is
// reported as not established instead of waiting in review forever.
void TestSpentBudgetIsReportedAsNotEstablished()
{
    SessionFixture fixture("delegation-budget");
    fixture.backend.SetReviewerTokens(30000);
    fixture.backend.SetReport("My agents ran out of budget before anything could be verified.");
    fixture.backend.SetChat(
        [](const Json& wire)
        {
            if (ToolMessages(wire).empty() && Offers(wire, "delegate_task"))
                return std::vector<Json>{Call(0, "d", "delegate_task", {{"objective", "Check the fixture port."}, {"reason", "Separate check."}}, true)};
            return std::vector<Json>{Text("Handed off.")};
        });
    Check(fixture.Say("Use your agents to check the fixture port.").succeeded, "The delegating turn failed.");
    const bool settled = fixture.SettlesAs(DelegatedTaskStatus::NotEstablished);
    Check(settled, "A run out of budget before its acceptance check was never reported; " + fixture.Describe());
    const auto report = fixture.backend.Requests(DelegationBackend::Kind::Report);
    Check(report.size() == 1 && report[0].dump().find("spent its budget") != std::string::npos &&
              fixture.backend.Requests(DelegationBackend::Kind::Parent).empty() &&
              fixture.session.AgentWorkflowSnapshot().state != revia::agents::WorkflowState::Accepted,
        "The budget outcome was not reported honestly, or the parent check still ran.");
}

// The outcome belongs to the chat it came from: after a new chat it is left in Agent Studio
// rather than reported into a conversation that never asked for it.
void TestOutcomeStaysOutOfAnotherChat()
{
    SessionFixture fixture("delegation-new-chat");
    fixture.backend.HoldWorkers();
    fixture.backend.SetChat(
        [](const Json& wire)
        {
            if (ToolMessages(wire).empty() && Offers(wire, "delegate_task"))
                return std::vector<Json>{Call(0, "d", "delegate_task", {{"objective", "Check the fixture port."}, {"reason", "Separate check."}}, true)};
            return std::vector<Json>{Text("Handed off.")};
        });
    Check(fixture.Say("Use your agents to check the fixture port.").succeeded, "The delegating turn failed.");
    Check(fixture.backend.WaitForHeldWorkers(2, 10s), "The workers did not start.");
    std::string error;
    Check(fixture.session.StartNewConversation(error), "A new chat could not start: " + error);
    fixture.backend.ReleaseWorkers();
    const bool settled = fixture.SettlesAs(DelegatedTaskStatus::ReportFailed);
    Check(settled, "An outcome from a closed chat was not left in Agent Studio; " + fixture.Describe());
    (void)fixture.PollUntil([] { return false; }, 1s);
    Check(fixture.Reports().empty() && fixture.backend.Requests(DelegationBackend::Kind::Report).empty() &&
              fixture.session.AgentWorkflowSnapshot().state == revia::agents::WorkflowState::AwaitingAcceptance,
        "An outcome was reported into a chat that did not ask for it, or was accepted without her report.");
}

// "Cancel the task" reaches a task whose outcome is waiting for the conversation, and that
// outcome is then never reported.
void TestTaskCommandCancelsAWaitingReport()
{
    SessionFixture fixture("delegation-task-command");
    fixture.backend.SetChat(
        [](const Json& wire)
        {
            if (ToolMessages(wire).empty() && Offers(wire, "delegate_task"))
                return std::vector<Json>{Call(0, "d", "delegate_task", {{"objective", "Check the fixture port."}, {"reason", "Separate check."}}, true)};
            return std::vector<Json>{Text("Handed off.")};
        });
    Check(fixture.Say("Use your agents to check the fixture port.").succeeded, "The delegating turn failed.");
    auto foreground = ReviaSessionTestAccess::HoldForeground(fixture.session);
    Check(fixture.HeldAtReporting(), "The outcome never became ready; " + fixture.Describe());
    const auto cancelled = ReviaSessionTestAccess::HandleCommand(fixture.session, "/task cancel");
    Check(cancelled.succeeded && cancelled.text == "Stopping the task." && fixture.Status() == DelegatedTaskStatus::Cancelled,
        "/task cancel did not reach a task waiting to report: " + cancelled.text);
    foreground.unlock();
    (void)fixture.PollUntil([] { return false; }, 1500ms);
    Check(fixture.Reports().empty(), "A task cancelled while its report waited was reported anyway.");
}

// Agent Studio retrying a task she reported as not established makes a new revision, and
// that revision's verified result is reported and accepted.
void TestStudioRetryReopensTheTask()
{
    SessionFixture fixture("delegation-retry");
    fixture.backend.SetWorkersVerified(false);
    fixture.backend.SetChat(
        [](const Json& wire)
        {
            if (ToolMessages(wire).empty() && Offers(wire, "delegate_task"))
                return std::vector<Json>{Call(0, "d", "delegate_task", {{"objective", "Check the fixture port."}, {"reason", "Separate check."}}, true)};
            return std::vector<Json>{Text("Handed off.")};
        });
    Check(fixture.Say("Use your agents to check the fixture port.").succeeded, "The delegating turn failed.");
    bool settled = fixture.SettlesAs(DelegatedTaskStatus::NotEstablished);
    Check(settled, "The unverified first run was not reported; " + fixture.Describe());
    ReviaSessionTestAccess::JoinWorkflow(fixture.session);
    fixture.backend.SetWorkersVerified(true);
    std::string error;
    Check(fixture.session.RetryAgentNode("analysis", "Check fixture/service.conf for the port.", "fixture-retry-evidence", error) &&
              fixture.session.DelegatedTask()->revision == 2,
        "A Studio retry did not reopen the task as a new revision: " + error);
    (void)fixture.PollUntil(
        [&]
        {
            const auto task = fixture.session.DelegatedTask();
            return !task || (task->revision == 2 && !InFlight(task->status));
        });
    const auto retried = fixture.session.DelegatedTask();
    settled = retried && retried->status == DelegatedTaskStatus::NotEstablished && retried->revision == 2;
    // The other worker failed too and was not retried, so revision 2 stops unverified again;
    // what matters is that it was reported afresh, once more, and never as revision 1.
    Check(settled && fixture.Reports().size() == 2, "The retried revision was not reported afresh; " + fixture.Describe());
}

// Simple conversation stays as fast as it was: On request offers nothing new, and a turn
// that does not delegate makes exactly one model request.
void TestOrdinaryConversationIsUnchanged()
{
    SessionFixture fixture("delegation-ordinary");
    const auto answered = fixture.Say("What do you think of that character?");
    const auto chat = fixture.backend.Requests(DelegationBackend::Kind::Chat);
    // Besides the reply itself, only the JSON review and memory checks every ordinary turn
    // already makes: no warmup, no classifier, no workflow and no report.
    const bool nothingElse = fixture.backend.Requests(DelegationBackend::Kind::Plain).empty() &&
                             fixture.backend.Requests(DelegationBackend::Kind::Report).empty() &&
                             fixture.backend.Requests(DelegationBackend::Kind::Worker).empty() &&
                             fixture.backend.Requests(DelegationBackend::Kind::Reviewer).empty() &&
                             fixture.backend.Requests(DelegationBackend::Kind::Parent).empty() &&
                             fixture.backend.Requests(DelegationBackend::Kind::ToolProposal).empty();
    Check(answered.succeeded && chat.size() == 1 && nothingElse && !Offers(chat[0], "delegate_task") && !fixture.session.DelegatedTask(),
        "An ordinary turn in On request mode was offered delegation or made extra model requests.");
    ReviaSessionTestAccess::SetDelegationMode(fixture.session, DelegationMode::Auto);
    Check(fixture.Say("Why does the audio keep cutting out?").succeeded &&
              Offers(fixture.backend.Requests(DelegationBackend::Kind::Chat).back(), "delegate_task"),
        "Auto did not let her choose delegation.");
    Check(fixture.Say("Answer this directly: why does the audio keep cutting out?").succeeded &&
              !Offers(fixture.backend.Requests(DelegationBackend::Kind::Chat).back(), "delegate_task"),
        "\"Answer this directly\" still offered delegation.");
    ReviaSessionTestAccess::SetDelegationMode(fixture.session, DelegationMode::Off);
    Check(fixture.Say("Use your agents to look into it.").succeeded &&
              !Offers(fixture.backend.Requests(DelegationBackend::Kind::Chat).back(), "delegate_task") &&
              fixture.backend.Requests(DelegationBackend::Kind::Worker).empty(),
        "Off still offered delegation or started workers.");
}

// Dropping the report's original TaskContract admission would disclose a report after
// recognition consent is withdrawn, even though its audience revision did not change.
// The upstream private voice context is simulated; workflow, report generation, archive,
// consent registry and queued speech use the real owners. No audio workers run.
void TestHeldReportRetainsOriginalConsentAdmission()
{
    for (const bool revoked : {false, true})
    {
        SessionFixture fixture(revoked ? "delegation-report-revoked" : "delegation-report-current");
        fixture.backend.HoldWorkers();
        fixture.backend.HoldReports();
        fixture.backend.SetChat(
            [](const Json& wire)
            {
                if (ToolMessages(wire).empty() && Offers(wire, "delegate_task"))
                    return std::vector<Json>{Call(0, "d", "delegate_task",
                        {{"objective", "Check the fixture port."}, {"reason", "Separate check."}}, true)};
                return std::vector<Json>{Text("Handed off.")};
            });
        const std::string archiveId = "held-report-admission";
        ReviaSessionTestAccess::OpenConversationArchive(fixture.session, fixture.temporary.root / "report-archive.db", archiveId);
        const auto input = ReviaSessionTestAccess::ConsentedDelegationInput(
            fixture.session, "Use your agents to check the fixture port.");
        Check(ReviaSessionTestAccess::RunAcceptedTurn(fixture.session, input).succeeded,
            "The consented delegation admission control did not complete its foreground reply.");
        Check(fixture.backend.WaitForHeldWorkers(2, 10s), "The report-admission workers did not reach their held requests.");
        auto& speech = ReviaSessionTestAccess::Speech(fixture.session);
        revia::speech::SpeechServiceTestAccess::ConfigureWithoutWorkers(speech, fixture.temporary.root / "speech");
        fixture.backend.ReleaseWorkers();
        Check(fixture.PollUntil([&] { return fixture.backend.WaitForHeldReports(1, 0ms); }, 20s),
            "The report did not reach the held generation request; " + fixture.Describe());
        const auto original = ReviaSessionTestAccess::DelegationContract(fixture.session);
        Check(original && original->scope.participantSource == revia::identity::SpeakerSource::ConsentedVoice &&
                  original->scope.consentRevision == input.context.consentRevision,
            "The delegated report did not retain the original consent-bearing task contract.");
        const auto audienceRevision = fixture.session.Audience().revision;
        if (revoked)
            ReviaSessionTestAccess::RevokeDelegationConsent(fixture.session, input.context.participantId);
        Check(fixture.session.Audience().revision == audienceRevision,
            "The consent regression changed audience revision and no longer isolates original contract admission.");
        fixture.backend.ReleaseReports();
        const bool settled = fixture.SettlesAs(revoked ? DelegatedTaskStatus::ReportFailed : DelegatedTaskStatus::Reported);
        const auto reports = fixture.Reports();
        const auto archived = ReviaSessionTestAccess::RawArchivedSession(fixture.session, archiveId);
        const auto queued = revia::speech::SpeechServiceTestAccess::QueuedTexts(speech);
        const bool archivedReport = std::any_of(archived.begin(), archived.end(), [](const auto& turn)
            { return turn.role == "assistant" && turn.content.find("fixture/service.conf") != std::string::npos; });
        const bool queuedReport = std::any_of(queued.begin(), queued.end(), [](const auto& text)
            { return text.find("fixture/service.conf") != std::string::npos; });
        const bool accepted = fixture.session.AgentWorkflowSnapshot().state == revia::agents::WorkflowState::Accepted;
        const std::string diagnostics = fixture.Describe() + "; reports=" + std::to_string(reports.size()) +
            "; archivedReport=" + std::to_string(archivedReport) + "; queuedReport=" + std::to_string(queuedReport) +
            "; accepted=" + std::to_string(accepted);
        if (revoked)
            Check(settled && reports.empty() && !archivedReport && !queuedReport && !accepted,
                "A report disclosed after its original recognition consent was revoked: " + diagnostics);
        else
            Check(settled && reports.size() == 1 && archivedReport && queuedReport && accepted,
                "The unchanged report admission control did not deliver, archive and queue speech: " + diagnostics);
        speech.StopSpeaking();
    }
}

// A synchronous presentation listener may cancel the exact task while its report is
// being published. The report tail must retain that terminal decision rather than
// claiming acceptance merely because the workflow no longer awaits acceptance.
void TestCancelDuringReportPublication()
{
    for (const bool cancelOnPublication : {false, true})
    {
        SessionFixture fixture(cancelOnPublication ? "delegation-publication-cancel" : "delegation-publication-current");
        fixture.backend.HoldWorkers();
        fixture.backend.SetChat(
            [](const Json& wire)
            {
                if (ToolMessages(wire).empty() && Offers(wire, "delegate_task"))
                    return std::vector<Json>{Call(0, "d", "delegate_task",
                        {{"objective", "Check the fixture port."}, {"reason", "Separate check."}}, true)};
                return std::vector<Json>{Text("Handed off.")};
            });
        Check(fixture.Say("Use your agents to check the fixture port.").succeeded, "The publication fixture could not delegate.");
        Check(fixture.backend.WaitForHeldWorkers(2, 10s), "The publication fixture workers did not reach their held requests.");
        const auto submitted = fixture.session.DelegatedTask();
        Check(submitted.has_value(), "The publication fixture lost its delegated task.");
        const std::string taskId = submitted->taskId;
        std::atomic<bool> callbackCompleted = false;
        std::atomic<unsigned> publications = 0;
        bool cancelled = false;
        bool cancelledRecordObserved = false;
        bool cancelledWorkflowObserved = false;
        std::string cancellationError;
        Subscription duringPublication{fixture.session.Events(), fixture.session.Events().Subscribe(
            [&](const RuntimeEvent& event)
            {
                if (event.kind != RuntimeEventKind::AssistantMessage || event.component != "Delegation" || event.phase != taskId)
                    return;
                ++publications;
                if (cancelOnPublication)
                {
                    cancelled = fixture.session.CancelDelegatedTask(taskId, cancellationError);
                    const auto record = fixture.session.DelegatedTask();
                    cancelledRecordObserved = record && record->status == DelegatedTaskStatus::Cancelled;
                    cancelledWorkflowObserved =
                        fixture.session.AgentWorkflowSnapshot().state == revia::agents::WorkflowState::Cancelled;
                }
                callbackCompleted.store(true);
            })};
        fixture.backend.ReleaseWorkers();
        Check(fixture.PollUntil([&] { return callbackCompleted.load(); }, 20s),
            "The publication fixture never published the report; " + fixture.Describe());
        // Polling for Cancelled here could pass before the report tail overwrites it.
        // Joining that tail establishes its final record without ending the session.
        ReviaSessionTestAccess::JoinDelegationReportWorker(fixture.session);
        const auto final = fixture.session.DelegatedTask();
        const auto workflow = fixture.session.AgentWorkflowSnapshot();
        Json persisted;
        std::ifstream(fixture.session.Paths().Resolve("RuntimeData/Agents/delegation.json")) >> persisted;
        const auto events = fixture.Snapshot();
        const bool claimedReported = std::any_of(events.begin(), events.end(), [&](const auto& event)
            { return event.kind == RuntimeEventKind::AgentWorkflow && event.component == "Delegation" && event.detail == taskId &&
                     event.phase == "Reported"; });
        const std::string diagnostics = fixture.Describe() + "; publications=" + std::to_string(publications.load()) +
            "; cancellationSucceeded=" + std::to_string(cancelled) + "; persisted=" + persisted.value("status", "missing") +
            "; claimedReported=" + std::to_string(claimedReported) + "; cancellationError=" + cancellationError;
        if (cancelOnPublication)
            Check(cancelled && cancelledRecordObserved && cancelledWorkflowObserved && publications.load() == 1 && final &&
                      final->status == DelegatedTaskStatus::Cancelled && !final->reportedArtifact &&
                      workflow.state == revia::agents::WorkflowState::Cancelled && persisted.value("status", "") == "Cancelled" &&
                      !claimedReported,
                "Publication-time cancellation was overwritten as a reported/accepted task: " + diagnostics);
        else
            Check(publications.load() == 1 && final && final->status == DelegatedTaskStatus::Reported && final->reportedArtifact &&
                      workflow.state == revia::agents::WorkflowState::Accepted && persisted.value("status", "") == "Reported" &&
                      claimedReported,
                "The normal publication control was not reported and accepted: " + diagnostics);
    }
}
}

void RunDelegationTests()
{
    if (std::getenv("REVIA_DELEGATION_CANCEL_ONLY"))
    {
        TestCancelDuringReportPublication();
        std::cout << "Delegated report publication-cancellation checks passed.\n";
        return;
    }
    if (std::getenv("REVIA_DELEGATION_ORIGIN_ONLY"))
    {
        TestHeldReportRetainsOriginalConsentAdmission();
        std::cout << "Delegated report original-admission checks passed.\n";
        return;
    }
    TestTheCueComesFromThePersonsOwnWords();
    TestOfferingFollowsModeAndWords();
    TestRegistryOffersDelegationOnlyWhenAllowed();
    TestDelegationSettingIsValidatedAndLive();
    TestWorkflowAcceptsOnlyTheExaminedCandidate();
    TestNarrowingTasksDoNotRevokeOtherWork();
    TestChatDelegationReportsThroughHerReply();
    TestCancelReachesOnlyTheNamedTask();
    TestStaleOutcomesAreNeverReported();
    TestUnverifiedWorkIsReportedAsNotEstablished();
    TestToolWorkersAreReadOnlyAndKeepTheReply();
    TestGoalTasksKeepTheReply();
    TestWorkersYieldToTheConversation();
    TestSpentBudgetIsReportedAsNotEstablished();
    TestOutcomeStaysOutOfAnotherChat();
    TestTaskCommandCancelsAWaitingReport();
    TestStudioRetryReopensTheTask();
    TestOrdinaryConversationIsUnchanged();
    TestHeldReportRetainsOriginalConsentAdmission();
    TestCancelDuringReportPublication();
    std::cout << "Chat delegation checks passed.\n";
}
