#include "Runtime/reviaSession.h"

#include "Core/utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

// Chat delegation: Revia handing one bounded investigation from the conversation to the
// existing Agent Studio workflow, and reporting what it found through her own reply path.
// There is no second agent manager here. The workflow owner runs the work; this file only
// records where a chat task came from, decides when its outcome is ready to report, and
// keeps a stale outcome from ever being shown or spoken.
namespace revia::runtime
{
namespace
{
using Json = nlohmann::json;

// Generous enough for a local 4B model to read, verify and review; still a hard ceiling,
// and spent time is never refunded by a retry.
agents::WorkflowBudget DelegationBudget()
{
    agents::WorkflowBudget budget;
    budget.maximumActiveMilliseconds = 300000;
    return budget;
}

constexpr std::size_t MaximumUserRequestCharacters = 2000;
// How long a task stays something task_status and cancel_task can name.
constexpr auto KnownTaskWindow = std::chrono::hours{1};
// Attempts at writing the report before the outcome is left to Agent Studio. A report the
// person's own new message preempted does not count: the foreground always comes first.
constexpr unsigned MaximumReportAttempts = 3;

bool Active(const DelegatedTaskStatus status)
{
    return status == DelegatedTaskStatus::Working || status == DelegatedTaskStatus::Reviewing || status == DelegatedTaskStatus::Reporting;
}

std::string Trimmed(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string OneLine(std::string value)
{
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

std::string Reference(const agents::ArtifactReference& reference)
{
    return reference.nodeId + " / " + reference.id + " v" + std::to_string(reference.version) + " sha256=" + reference.hash;
}

agents::ArtifactReference ReferenceTo(const std::string& nodeId, const agents::WorkflowArtifact& artifact)
{
    return {nodeId, artifact.id, artifact.version, artifact.hash};
}

bool Same(const agents::ArtifactReference& left, const agents::ArtifactReference& right)
{
    return left.nodeId == right.nodeId && left.id == right.id && left.version == right.version && left.hash == right.hash;
}

const agents::WorkflowNodeSnapshot* Parent(const agents::WorkflowSnapshot& snapshot)
{
    const auto found = std::find_if(
        snapshot.nodes.begin(), snapshot.nodes.end(), [](const auto& node) { return node.role == agents::WorkflowRole::Parent; });
    return found == snapshot.nodes.end() ? nullptr : &*found;
}

std::string SectionTitle(const agents::DeliverableSection section)
{
    switch (section)
    {
    case agents::DeliverableSection::Steps:
        return "What was found";
    case agents::DeliverableSection::Constraints:
        return "What the evidence covers and does not";
    case agents::DeliverableSection::Risks:
        return "Other explanations";
    case agents::DeliverableSection::AcceptanceCriteria:
        return "What would settle it";
    }
    return "Details";
}

// The reviewed deliverable as Revia examines it: every section, the evidence with the host's
// own receipts, and the exact identities of what each role produced.
std::string DeliverableEvidence(const agents::WorkflowArtifact& artifact, const agents::WorkflowSnapshot& snapshot)
{
    std::string text = "Reviewed deliverable: " + Reference(ReferenceTo("parent", artifact)) + "\n";
    try
    {
        const auto payload = Json::parse(artifact.content);
        text += "Summary: " + payload.value("summary", std::string{}) + "\n";
        text += "Evidence: " + payload.value("evidence", std::string{}) + "\n";
        for (const auto section : {agents::DeliverableSection::Steps, agents::DeliverableSection::Constraints,
                 agents::DeliverableSection::Risks, agents::DeliverableSection::AcceptanceCriteria})
        {
            const auto name = agents::ToString(section);
            if (!payload.contains(name) || !payload.at(name).is_object())
                continue;
            text += SectionTitle(section) + ":\n";
            for (const auto& item : payload.at(name).value("items", Json::array()))
                if (item.is_string())
                    text += "- " + item.get<std::string>() + "\n";
            const auto reason = payload.at(name).value("noneReason", std::string{});
            if (!reason.empty())
                text += "- (none) " + reason + "\n";
        }
    }
    catch (...)
    {
        text += "The deliverable's sections could not be read; only its identity above is reliable.\n";
    }
    text += "Produced by:";
    for (const auto& node : snapshot.nodes)
    {
        if (node.attempts.empty())
            continue;
        const auto& attempt = node.attempts.back();
        text += "\n- " + node.workingOn + ": " + agents::ToString(attempt.state) + (attempt.verified ? ", verified" : ", unverified") +
                (attempt.artifact ? ", " + Reference(*attempt.artifact) : std::string{}) +
                (attempt.reportedModel ? ", model " + *attempt.reportedModel : std::string{});
    }
    return text;
}

// An outcome without a verified deliverable: which stage stopped and the host's safe reason,
// so she can say plainly that nothing was established instead of inventing a finding.
std::string UnverifiedEvidence(const agents::WorkflowSnapshot& snapshot)
{
    std::string text = snapshot.state == agents::WorkflowState::Exhausted
                           ? "The workflow stopped at its budget before the work was verified.\n"
                           : "The workflow stopped before the work was verified.\n";
    for (const auto& node : snapshot.nodes)
    {
        text += "- " + node.workingOn + ": " + agents::ToString(node.state);
        if (!node.attempts.empty())
        {
            const auto& attempt = node.attempts.back();
            text += " (attempt " + std::to_string(attempt.ordinal) + ", " + (attempt.verified ? "verified" : "unverified") +
                    (attempt.diagnostic.empty() ? std::string{} : "; " + OneLine(attempt.diagnostic)) + ")";
        }
        text += "\n";
    }
    text += "No finding from this run is established. Its full record is in Agent Studio, where it can be retried with changed input.";
    return text;
}

std::string Stage(const agents::WorkflowSnapshot& snapshot)
{
    std::vector<std::string> running, waiting;
    for (const auto& node : snapshot.nodes)
    {
        if (node.state == agents::WorkflowState::Running)
            running.push_back(node.workingOn);
        else if (node.state == agents::WorkflowState::Waiting)
            waiting.push_back(node.workingOn);
    }
    std::string stage;
    for (std::size_t index = 0; index < running.size(); ++index)
        stage += (index == 0 ? "" : " and ") + running[index];
    if (!stage.empty())
        stage += running.size() == 1 ? " is working" : " are working";
    if (!waiting.empty())
        stage += (stage.empty() ? "" : "; ") + waiting.front() + " is next";
    return stage.empty() ? agents::ToString(snapshot.state) : stage;
}

std::string Minutes(const std::chrono::steady_clock::time_point since)
{
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now() - since).count();
    return minutes < 1 ? std::string("under a minute") : std::to_string(minutes) + " min";
}

bool SaveJson(const std::filesystem::path& path, const Json& value)
{
    const std::filesystem::path temporary = path.string() + ".tmp";
    try
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(temporary, std::ios::trunc);
        output << value.dump(2);
        output.flush();
        if (!output.good())
            return false;
        output.close();
        if (output.fail())
            return false;
#ifdef _WIN32
        return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::filesystem::rename(temporary, path);
        return true;
#endif
    }
    catch (...)
    {
        return false;
    }
}

Json Submitted(const DelegatedTaskView& view, const bool already)
{
    return {{"task_id", view.taskId}, {"status", already ? "already_submitted" : "submitted"},
        {"workers", "an investigation worker and a verification worker, then an independent reviewer"}, {"access", view.access},
        {"note", already ? "You already handed this off on this turn; do not delegate it again."
                         : "Nothing has been checked yet. Tell the person briefly what you handed off and that you will report here when "
                           "the review finishes. Do not wait or poll for it."}};
}
}

std::string ToString(const DelegatedTaskStatus status)
{
    switch (status)
    {
    case DelegatedTaskStatus::Working:
        return "Working";
    case DelegatedTaskStatus::Reviewing:
        return "Reviewing";
    case DelegatedTaskStatus::Reporting:
        return "Reporting";
    case DelegatedTaskStatus::Reported:
        return "Reported";
    case DelegatedTaskStatus::NotEstablished:
        return "Not established";
    case DelegatedTaskStatus::Cancelled:
        return "Cancelled";
    case DelegatedTaskStatus::Rejected:
        return "Rejected";
    case DelegatedTaskStatus::Replaced:
        return "Replaced";
    case DelegatedTaskStatus::ReportFailed:
        return "Report unavailable";
    }
    return "Unknown";
}

agents::ToolExecution ReviaSession::DelegateFromChat(const agents::DelegationSubmission& submission, const agents::ToolCallContext& call)
{
    if (!call.Admitted())
        return agents::ToolExecution::Fail("The turn ended before anything was handed off.");
    if (delegationMode.load() == agents::DelegationMode::Off)
        return agents::ToolExecution::Fail("Delegation is turned off, so nothing was handed off. Answer directly instead.");
    const std::string objective = Trimmed(utf8::Prefix(utf8::Sanitize(submission.objective), agents::MaximumDelegatedObjectiveCharacters));
    if (objective.empty())
        return agents::ToolExecution::Fail("Say what the agents should investigate.");
    // One launch at a time; the turn that already owns the workforce is answered first.
    std::lock_guard launching(delegationLaunchMutex);
    const auto asker = CaptureTaskContext();
    {
        std::lock_guard lock(delegationMutex);
        if (delegation && sessionIdentity.IsCurrent(delegation->origin) && Active(delegation->view.status))
        {
            // The model asking twice on one turn, in one step or a later one, gets the task it
            // already started rather than a second copy of the work.
            if (submission.originTurnId != 0 && delegation->view.originTurnId == submission.originTurnId)
                return agents::ToolExecution::Ok(Submitted(delegation->view, true).dump());
            const bool disclosed = CanDiscloseTask(delegation->context, delegation->origin, asker);
            return agents::ToolExecution::Fail(
                disclosed ? "Your agents are still on task " + delegation->view.taskId + " (\"" + utf8::Prefix(delegation->view.objective, 160) +
                                "\"), so nothing new was handed off. Tell the person; they can wait for it, or ask you to cancel it first."
                          : "Your agents are busy with another task, so nothing was handed off. Answer directly instead.");
        }
    }
    const auto held = agentWorkflow.Snapshot();
    bool chatOwned = false;
    {
        std::lock_guard lock(delegationMutex);
        chatOwned = delegation && !held.id.empty() && delegation->view.taskId == held.id;
    }
    if (held.state == agents::WorkflowState::Running)
        return agents::ToolExecution::Fail("Agent Studio is running another workflow, so your agents are busy. Answer directly, or offer to "
                                           "try again when it finishes.");
    // The workflow owner holds one workflow, and starting another discards it. Work the person
    // still has to decide on in Agent Studio is never discarded from chat.
    if (!held.id.empty() && (held.state == agents::WorkflowState::AwaitingAcceptance ||
                                (!chatOwned && (held.state == agents::WorkflowState::Paused || held.state == agents::WorkflowState::Queued))))
        return agents::ToolExecution::Fail("Agent Studio is holding unfinished work that waits for the person's decision there, so nothing "
                                           "was handed off. Answer directly, and mention it can be finished or discarded in Agent Studio.");

    const auto capabilities = actionRuntime.IsInitialized() ? std::optional(actionRuntime.Settings()) : std::nullopt;
    const bool tools =
        capabilities && capabilities->mode != actions::ExecutionMode::Disabled && !capabilities->approvedRoots.empty();
    AgentLaunchOptions options;
    options.readOnlyTools = true;
    options.investigation = true;
    options.budget = DelegationBudget();
    const std::string id = actions::NewActionId();

    DelegationRecord record;
    record.view.taskId = id;
    record.view.objective = objective;
    record.view.reason = Trimmed(utf8::Prefix(utf8::Sanitize(submission.reason), agents::MaximumDelegationReasonCharacters));
    record.view.originTurnId = submission.originTurnId;
    record.view.conversationId = CurrentConversationId();
    record.view.access = tools ? "read-only: the workers may read and list files inside the approved folders and cannot change anything"
                               : "analysis only: no folders are approved, so the workers read nothing and reason from the objective";
    record.view.budget = options.budget;
    record.view.submittedAt = std::chrono::system_clock::now();
    record.view.submittedSteady = std::chrono::steady_clock::now();
    record.userRequest = utf8::Prefix(utf8::Sanitize(submission.userRequest), MaximumUserRequestCharacters);
    record.origin = sessionIdentity.Stamp(id, {}, companionAuthority->Revision());
    record.context = asker;
    record.memoryScope = CapturePrivateMemoryScope();
    // Recorded before the start, so the first snapshot the workflow publishes already
    // belongs to a chat task and never takes Agent Studio's automatic acceptance.
    std::optional<DelegationRecord> previous;
    {
        std::lock_guard lock(delegationMutex);
        previous = std::move(delegation);
        delegation = std::move(record);
    }
    std::string error;
    const bool launched =
        call.Admitted() && LaunchAgentWorkflow(id, objective, tools ? AgentProviderMode::LocalWithTools : AgentProviderMode::Local, options, error);
    if (!launched)
    {
        {
            std::lock_guard lock(delegationMutex);
            delegation = std::move(previous);
        }
        return agents::ToolExecution::Fail(
            error.empty() ? "The turn ended before anything was handed off." : "Nothing was handed off; the agents could not start: " + error);
    }
    StartDelegationWorker();
    PersistDelegation();
    PublishDelegation("Submitted", objective);
    DelegatedTaskView view;
    {
        std::lock_guard lock(delegationMutex);
        view = delegation->view;
    }
    appLogger.Log("Delegated task " + id + " from chat turn #" + std::to_string(submission.originTurnId) + " | access=" +
                  (tools ? "read-only tools" : "analysis only") + " | objective=" + OneLine(utf8::Prefix(objective, 200)));
    return agents::ToolExecution::Ok(Submitted(view, false).dump());
}

agents::ToolExecution ReviaSession::DelegatedTaskStatusForChat(const std::string& taskId, const agents::ToolCallContext& call) const
{
    if (!call.Admitted())
        return agents::ToolExecution::Fail("The turn ended before the task was checked.");
    const auto asker = CaptureTaskContext();
    const auto snapshot = agentWorkflow.Snapshot();
    std::lock_guard lock(delegationMutex);
    if (!delegation || !sessionIdentity.IsCurrent(delegation->origin) || !CanDiscloseTask(delegation->context, delegation->origin, asker) ||
        (!taskId.empty() && taskId != delegation->view.taskId))
        return agents::ToolExecution::Fail("No delegated task has that id.");
    const auto& view = delegation->view;
    Json status = {{"task_id", view.taskId}, {"status", ToString(view.status)}, {"objective", view.objective}, {"access", view.access},
        {"elapsed", Minutes(view.submittedSteady)}, {"revision", view.revision}};
    if (snapshot.id == view.taskId)
    {
        Json active = Json::array(), waiting = Json::array();
        for (const auto& node : snapshot.nodes)
        {
            if (node.state == agents::WorkflowState::Running)
                active.push_back({{"role", node.workingOn}, {"action", node.currentAction},
                    {"seconds", (node.activeMilliseconds + 500) / 1000}, {"next_handoff", node.nextHandoff}});
            else if (node.state == agents::WorkflowState::Waiting)
                waiting.push_back(node.workingOn);
        }
        status["workflow"] = agents::ToString(snapshot.state);
        status["active"] = active;
        status["waiting"] = waiting;
        status["budget"] = {{"requests", std::to_string(snapshot.requests) + " of " + std::to_string(view.budget.maximumRequests)},
            {"model_calls", std::to_string(snapshot.providerCalls) + " of " + std::to_string(view.budget.maximumProviderCalls)},
            {"tool_calls", std::to_string(snapshot.toolCalls) + " of " + std::to_string(view.budget.maximumToolCalls)}};
    }
    if (!view.detail.empty())
        status["detail"] = view.detail;
    status["note"] = Active(view.status) ? "Still in progress; nothing is established until the review finishes and you report it."
                                         : "This task is over; answer from its record.";
    return agents::ToolExecution::Ok(status.dump());
}

agents::ToolExecution ReviaSession::CancelDelegatedTaskForChat(const std::string& taskId, const agents::ToolCallContext& call)
{
    if (!call.Admitted())
        return agents::ToolExecution::Fail("The turn ended before the task was cancelled.");
    const auto asker = CaptureTaskContext();
    {
        std::lock_guard lock(delegationMutex);
        if (!delegation || !CanDiscloseTask(delegation->context, delegation->origin, asker))
            return agents::ToolExecution::Fail("No delegated task has that id.");
    }
    std::string error;
    if (!CancelDelegatedTask(taskId, error))
        return agents::ToolExecution::Fail(error);
    return agents::ToolExecution::Ok("Cancelled task " + taskId + ". Nothing more from it will be reported.");
}

bool ReviaSession::CancelDelegatedTask(const std::string& taskId, std::string& outError)
{
    {
        std::lock_guard lock(delegationMutex);
        if (!delegation || !sessionIdentity.IsCurrent(delegation->origin) || delegation->view.taskId != taskId)
        {
            outError = "No delegated task has that id.";
            return false;
        }
        if (!Active(delegation->view.status))
        {
            outError = "That task is already over (" + ToString(delegation->view.status) + "), so there is nothing to cancel.";
            return false;
        }
        delegation->view.status = DelegatedTaskStatus::Cancelled;
        delegation->view.detail = "Cancelled on request before it finished.";
        delegation->reportDue = false;
    }
    // Only this task's workflow: if anything else holds the workforce now, it is left alone.
    if (agentWorkflow.Snapshot().id == taskId)
        agentWorkflow.RequestCancel();
    delegationCondition.notify_all();
    PersistDelegation();
    PublishDelegation("Cancelled", "Cancelled on request.");
    appLogger.Log("Delegated task " + taskId + " cancelled on request.");
    outError.clear();
    return true;
}

std::optional<DelegatedTaskView> ReviaSession::DelegatedTask() const
{
    std::lock_guard lock(delegationMutex);
    if (!delegation || !sessionIdentity.IsCurrent(delegation->origin))
        return std::nullopt;
    return delegation->view;
}

bool ReviaSession::DelegatedTaskKnown() const
{
    const auto asker = CaptureTaskContext();
    std::lock_guard lock(delegationMutex);
    return delegation && sessionIdentity.IsCurrent(delegation->origin) && CanDiscloseTask(delegation->context, delegation->origin, asker) &&
           (Active(delegation->view.status) || std::chrono::steady_clock::now() - delegation->view.submittedSteady < KnownTaskWindow);
}

bool ReviaSession::AdvanceDelegatedWorkflow(const agents::WorkflowSnapshot& snapshot)
{
    std::string phase, message;
    bool evaluate = false, notify = false;
    {
        std::lock_guard lock(delegationMutex);
        if (!delegation || snapshot.id.empty() || delegation->view.taskId != snapshot.id)
            return false;
        // A record from a session that has ended no longer owns anything.
        if (!sessionIdentity.IsCurrent(delegation->origin))
            return false;
        auto& record = *delegation;
        auto& view = record.view;
        const auto state = snapshot.state;
        const auto reopen = [&]()
        {
            // Agent Studio restarted this same task (a retry with changed evidence, or a
            // resume): its next outcome is a new revision, reported afresh.
            ++view.revision;
            view.status = DelegatedTaskStatus::Working;
            view.detail.clear();
            view.reportedArtifact.reset();
            record.reportDue = false;
            record.verifiedOutcome = false;
            record.candidate.reset();
            record.outcomeEvidence.clear();
            record.reportAttempts = 0;
            phase = "Reopened";
            message = "Agent Studio restarted this task as revision " + std::to_string(view.revision) + ".";
        };
        switch (view.status)
        {
        case DelegatedTaskStatus::Reported:
        case DelegatedTaskStatus::Rejected:
        case DelegatedTaskStatus::Replaced:
            return true;
        case DelegatedTaskStatus::NotEstablished:
        case DelegatedTaskStatus::Cancelled:
        case DelegatedTaskStatus::ReportFailed:
            if (state != agents::WorkflowState::Running)
                return true;
            reopen();
            break;
        default:
            break;
        }
        if (state == agents::WorkflowState::Cancelled)
        {
            view.status = DelegatedTaskStatus::Cancelled;
            view.detail = "The workflow was cancelled before its outcome was reported.";
            record.reportDue = false;
            phase = "Cancelled";
            message = view.detail;
        }
        else if (state == agents::WorkflowState::Rejected)
        {
            view.status = DelegatedTaskStatus::Rejected;
            view.detail = "Rejected in Agent Studio before it was reported.";
            record.reportDue = false;
            phase = "Rejected";
            message = view.detail;
        }
        else if (state == agents::WorkflowState::Running || state == agents::WorkflowState::Queued)
        {
            if (view.status == DelegatedTaskStatus::Reporting && !record.verifiedOutcome)
                reopen();
            else if (view.status == DelegatedTaskStatus::Reviewing)
                view.status = DelegatedTaskStatus::Working;
        }
        else if (state == agents::WorkflowState::AwaitingAcceptance || state == agents::WorkflowState::Accepted)
        {
            if (view.status != DelegatedTaskStatus::Reporting)
            {
                const auto* parent = Parent(snapshot);
                if (state == agents::WorkflowState::AwaitingAcceptance && parent && parent->attempts.empty())
                {
                    // The native acceptance check, exactly as Agent Studio runs it. It is the
                    // gate; her report afterwards is what makes the work accepted.
                    view.status = DelegatedTaskStatus::Reviewing;
                    evaluate = true;
                }
                else if (const auto candidate = state == agents::WorkflowState::Accepted ? agentWorkflow.AcceptedArtifact()
                                                                                          : agentWorkflow.CandidateArtifact())
                {
                    record.candidate = candidate;
                    record.verifiedOutcome = true;
                    record.outcomeEvidence = DeliverableEvidence(*candidate, snapshot);
                    record.reportDue = true;
                    view.status = DelegatedTaskStatus::Reporting;
                    notify = true;
                    phase = "Reviewed";
                    message = "The review verified the deliverable; Revia reports it when the conversation is free.";
                }
                else
                {
                    view.status = DelegatedTaskStatus::Reviewing;
                }
            }
        }
        else if (state == agents::WorkflowState::Paused || state == agents::WorkflowState::Exhausted)
        {
            if (view.status != DelegatedTaskStatus::Reporting)
            {
                record.candidate.reset();
                record.verifiedOutcome = false;
                record.outcomeEvidence = UnverifiedEvidence(snapshot);
                record.reportDue = true;
                view.status = DelegatedTaskStatus::Reporting;
                notify = true;
                phase = "Unverified";
                message = "The workflow stopped without verified evidence; Revia reports that when the conversation is free.";
            }
        }
    }
    if (evaluate)
    {
        // Asynchronous: the parent attempt runs on the workflow's own thread. A refusal while
        // the previous run is still tearing down is retried on the next poll.
        std::string error;
        (void)agentWorkflow.EvaluateParent(error);
    }
    if (notify)
        delegationCondition.notify_all();
    if (!phase.empty())
    {
        PersistDelegation();
        PublishDelegation(phase, message);
    }
    return true;
}

void ReviaSession::RetireDelegationFor(const std::string& newWorkflowId)
{
    {
        std::lock_guard lock(delegationMutex);
        if (!delegation || delegation->view.taskId == newWorkflowId || delegation->view.status == DelegatedTaskStatus::Replaced ||
            delegation->view.status == DelegatedTaskStatus::Reported || delegation->view.status == DelegatedTaskStatus::Rejected)
            return;
        delegation->view.status = DelegatedTaskStatus::Replaced;
        delegation->view.detail = "Agent Studio started another workflow, which took the workers.";
        delegation->reportDue = false;
    }
    delegationCondition.notify_all();
    PersistDelegation();
    PublishDelegation("Replaced", "Agent Studio started another workflow, which took the workers.");
}

void ReviaSession::CancelDelegation(const std::string& because)
{
    std::string taskId;
    {
        std::lock_guard lock(delegationMutex);
        if (!delegation || !Active(delegation->view.status))
            return;
        delegation->view.status = DelegatedTaskStatus::Cancelled;
        delegation->view.detail = "Cancelled because " + because + ".";
        delegation->reportDue = false;
        taskId = delegation->view.taskId;
    }
    delegationCondition.notify_all();
    PersistDelegation();
    PublishDelegation("Cancelled", "Cancelled because " + because + ".");
    appLogger.Log("Delegated task " + taskId + " cancelled: " + because);
}

std::string ReviaSession::DescribeDelegatedRunning() const
{
    const auto current = CaptureTaskContext();
    const auto snapshot = agentWorkflow.Snapshot();
    std::lock_guard lock(delegationMutex);
    if (!delegation || !CanDiscloseTask(delegation->context, delegation->origin, current))
        return {};
    const auto& view = delegation->view;
    if (view.status != DelegatedTaskStatus::Working && view.status != DelegatedTaskStatus::Reviewing)
        return {};
    return "'" + view.objective + "', handed to your agents " + Minutes(view.submittedSteady) + " ago; latest: " +
           (view.status == DelegatedTaskStatus::Reviewing ? std::string("your acceptance check is reviewing their work")
                                                          : snapshot.id == view.taskId ? Stage(snapshot) : std::string("starting"));
}

std::string ReviaSession::DescribeDelegatedFinished() const
{
    const auto current = CaptureTaskContext();
    std::lock_guard lock(delegationMutex);
    if (!delegation || !CanDiscloseTask(delegation->context, delegation->origin, current))
        return {};
    const auto& view = delegation->view;
    const std::string task = "Your agents' task '" + view.objective + "'";
    switch (view.status)
    {
    case DelegatedTaskStatus::Reporting:
        return task + (delegation->verifiedOutcome ? " finished and its deliverable was verified by review"
                                                   : " stopped without verified evidence") +
               "; you are telling the person what it found now.";
    case DelegatedTaskStatus::Reported:
        return task + " finished, was verified, and you already reported it.";
    case DelegatedTaskStatus::NotEstablished:
        return task + " did not establish an answer, and you already said so. It can be retried in Agent Studio.";
    case DelegatedTaskStatus::Cancelled:
        return task + " was cancelled. " + view.detail;
    case DelegatedTaskStatus::Rejected:
        return task + " was rejected by the person in Agent Studio.";
    case DelegatedTaskStatus::Replaced:
        return task + " was replaced by another Agent Studio workflow; nothing from it was reported.";
    case DelegatedTaskStatus::ReportFailed:
        return task + " finished, but you could not report it; its result is in Agent Studio. " + view.detail;
    default:
        return {};
    }
}

void ReviaSession::PersistDelegation()
{
    Json value;
    {
        std::lock_guard lock(delegationMutex);
        if (!delegation)
            return;
        const auto& view = delegation->view;
        value = {{"schema", 1}, {"taskId", view.taskId}, {"objective", view.objective}, {"reason", view.reason},
            {"originTurnId", view.originTurnId}, {"conversationId", view.conversationId}, {"access", view.access},
            {"status", ToString(view.status)}, {"revision", view.revision}, {"detail", view.detail},
            {"submittedAt",
                std::chrono::duration_cast<std::chrono::milliseconds>(view.submittedAt.time_since_epoch()).count()},
            {"budget", {{"requests", view.budget.maximumRequests}, {"activeMilliseconds", view.budget.maximumActiveMilliseconds},
                           {"providerCalls", view.budget.maximumProviderCalls}, {"toolCalls", view.budget.maximumToolCalls}}}};
        if (view.reportedArtifact)
            value["reportedArtifact"] = {{"node", view.reportedArtifact->nodeId}, {"id", view.reportedArtifact->id},
                {"version", view.reportedArtifact->version}, {"hash", view.reportedArtifact->hash}};
    }
    if (!SaveJson(companionPaths.Resolve("RuntimeData/Agents/delegation.json"), value))
        appLogger.Warning("The delegated task record could not be saved; Agent Studio still holds the workflow itself.");
}

void ReviaSession::PublishDelegation(const std::string& phase, const std::string& message) const
{
    RuntimeEvent event{RuntimeEventKind::AgentWorkflow, state.load(), message};
    {
        std::lock_guard lock(delegationMutex);
        if (!delegation)
            return;
        event.stamp = delegation->origin;
        event.conversationTurnId = delegation->view.originTurnId;
        event.detail = delegation->view.taskId;
    }
    event.component = "Delegation";
    event.phase = phase;
    eventBus.Publish(std::move(event));
}

void ReviaSession::StartDelegationWorker()
{
    std::lock_guard lock(delegationMutex);
    if (delegationWorker.joinable())
        return;
    delegationWorker = std::jthread([this](const std::stop_token stopToken) { RunDelegationReport(stopToken); });
}

void ReviaSession::StopDelegationWorker()
{
    std::jthread worker;
    {
        std::lock_guard lock(delegationMutex);
        worker = std::move(delegationWorker);
    }
    if (!worker.joinable())
        return;
    worker.request_stop();
    delegationCondition.notify_all();
    worker.join();
}

void ReviaSession::RunDelegationReport(const std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        DelegationRecord claimed;
        {
            std::unique_lock lock(delegationMutex);
            if (!delegationCondition.wait(lock, stopToken, [this] { return delegation && delegation->reportDue; }))
                return;
            delegation->reportDue = false;
            claimed = *delegation;
        }
        const std::string taskId = claimed.view.taskId;
        const std::uint32_t revision = claimed.view.revision;
        const bool verified = claimed.verifiedOutcome;
        const auto expected = claimed.candidate ? std::optional(ReferenceTo("parent", *claimed.candidate)) : std::nullopt;
        if (verified && !expected)
            continue;
        // Still the outcome that was claimed: same task, same revision, nothing cancelled or
        // replaced it, and this session still owns it.
        const auto current = [this, taskId, revision]()
        {
            std::lock_guard lock(delegationMutex);
            return delegation && delegation->view.taskId == taskId && delegation->view.revision == revision &&
                   delegation->view.status == DelegatedTaskStatus::Reporting && sessionIdentity.IsCurrent(delegation->origin);
        };
        // And the workflow still holds exactly what she is about to examine.
        const auto outcomeCurrent = [this, taskId, verified, expected]()
        {
            const auto snapshot = agentWorkflow.Snapshot();
            if (snapshot.id != taskId)
                return false;
            if (!verified)
                return snapshot.state == agents::WorkflowState::Paused || snapshot.state == agents::WorkflowState::Exhausted;
            const auto candidate = snapshot.state == agents::WorkflowState::Accepted ? agentWorkflow.AcceptedArtifact()
                                                                                      : agentWorkflow.CandidateArtifact();
            return candidate && Same(ReferenceTo("parent", *candidate), *expected);
        };
        const auto requeue = [this, taskId, revision](const bool counted, const std::string& reason)
        {
            bool failed = false;
            {
                std::lock_guard lock(delegationMutex);
                if (!delegation || delegation->view.taskId != taskId || delegation->view.revision != revision ||
                    delegation->view.status != DelegatedTaskStatus::Reporting)
                    return;
                if (counted && ++delegation->reportAttempts >= MaximumReportAttempts)
                {
                    delegation->view.status = DelegatedTaskStatus::ReportFailed;
                    delegation->view.detail = reason;
                    failed = true;
                }
                else
                    delegation->reportDue = true;
            }
            if (failed)
            {
                PersistDelegation();
                PublishDelegation("ReportFailed", reason);
                PublishComponent("Delegation", "Report unavailable",
                    "Revia could not report the delegated task; its outcome stays in Agent Studio. " + reason);
            }
        };

        // Her own conversation always comes first: wait for the turn in progress to finish,
        // for her to stop speaking, and for the person to stop talking.
        std::unique_lock operationLock(operationMutex, std::defer_lock);
        while (!stopToken.stop_requested() && current() && outcomeCurrent())
        {
            if (started.load() && !busy.load() && !speechService.HasPendingSpeech() && !speechRecognitionService.IsRecording() &&
                CanDiscloseTask(claimed.context, claimed.origin, CaptureTaskContext()) && operationLock.try_lock())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!operationLock.owns_lock())
            continue;
        // Rechecked under the conversation lane, which the wait did not hold.
        const auto audience = Audience();
        if (!started.load() || busy.load() || !current() || !outcomeCurrent() || audience.kind != identity::AudienceKind::Private ||
            !CanDiscloseTask(claimed.context, claimed.origin, CaptureTaskContext()))
        {
            operationLock.unlock();
            if (current() && outcomeCurrent())
                requeue(false, {});
            continue;
        }
        // Delivery is admitted only while the session, the audience it is spoken to and the
        // exact outcome all stay current. Quiet mode keeps her voice down, not the answer
        // the person asked for.
        const RuntimeStamp origin = claimed.origin;
        const std::function<bool()> admission = [this, origin, revision = audience.revision, current, outcomeCurrent]()
        {
            const auto now = Audience();
            return started.load() && Admits(origin) && now.kind == identity::AudienceKind::Private && now.revision == revision &&
                   current() && outcomeCurrent();
        };
        const bool speak = ShouldSpeakOnCurrentChannel() && !initiativeController.IsQuiet();
        bool preempted = false;
        SessionResult report = GuardTurn(
            [&]() -> SessionResult
            {
                busy.store(true);
                const std::stop_token operationToken = BeginOperation();
                if (!llmAvailable || (llamaServerProcess.WasStartedByRevia() && !llamaServerProcess.IsRunning()))
                {
                    PublishComponent("Language model", "Restarting", "The local model stopped; Revia is restarting it before reporting.");
                    llmAvailable = EnsureLLMAvailable(operationToken);
                }
                SessionResult written = conversationRuntime.ReportDelegatedTask(claimed.userRequest, claimed.view.objective,
                    claimed.outcomeEvidence, verified, profile, llmAvailable, speak, operationToken, audience.revision, admission,
                    claimed.memoryScope);
                if (written.succeeded && written.fromAssistant && !written.text.empty() && admission())
                    ArchiveTurn("assistant", written.text, claimed.memoryScope, admission);
                preempted = operationToken.stop_requested();
                busy.store(false);
                return written;
            });
        operationLock.unlock();

        const bool delivered = report.succeeded && report.fromAssistant && !report.text.empty() && admission();
        if (!delivered)
        {
            requeue(!preempted, report.reason.empty() ? "The report could not be written." : report.reason);
            if (!preempted)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        RuntimeEvent message;
        message.kind = RuntimeEventKind::AssistantMessage;
        message.stamp = origin;
        message.audienceRevision = report.audienceRevision;
        message.state = state.load();
        message.component = "Delegation";
        message.phase = taskId;
        message.message = report.text;
        message.detail = report.reasoning;
        message.turnId = report.speechPending ? report.utteranceId : 0;
        eventBus.Publish(std::move(message));

        // Accepted only now, and only the deliverable she examined.
        std::string acceptance;
        bool accepted = true;
        if (verified && agentWorkflow.Snapshot().state == agents::WorkflowState::AwaitingAcceptance)
            accepted = agentWorkflow.AcceptCandidate(*expected, acceptance);
        std::string phase;
        {
            std::lock_guard lock(delegationMutex);
            if (delegation && delegation->view.taskId == taskId && delegation->view.revision == revision)
            {
                auto& view = delegation->view;
                if (!verified)
                {
                    view.status = DelegatedTaskStatus::NotEstablished;
                    view.detail = "Reported that the review did not verify an answer.";
                }
                else if (accepted)
                {
                    view.status = DelegatedTaskStatus::Reported;
                    view.reportedArtifact = expected;
                    view.detail = "Reported and accepted " + Reference(*expected) + ".";
                }
                else
                {
                    view.status = DelegatedTaskStatus::ReportFailed;
                    view.detail = "Reported, but acceptance was refused: " + acceptance;
                }
                phase = view.status == DelegatedTaskStatus::Reported         ? "Reported"
                        : view.status == DelegatedTaskStatus::NotEstablished ? "NotEstablished"
                                                                              : "ReportFailed";
            }
        }
        if (!phase.empty())
        {
            PersistDelegation();
            PublishDelegation(phase, report.text);
        }
        appLogger.Log("Delegated task " + taskId + " reported (" + (verified ? "verified" : "not established") + ")" +
                      (accepted ? "" : "; acceptance refused: " + acceptance));
    }
}

} // namespace revia::runtime
