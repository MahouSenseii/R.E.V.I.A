#include "Runtime/reviaSession.h"
#include "Core/taskContract.h"
#include "Audit/contentDigest.h"

namespace revia::runtime
{
namespace
{
memory::MemoryScope InputScope(const agents::InputContext& input)
{
    return {input.participantId, input.audience, input.participantSource, input.consentRevision, input.stamp.companionId};
}

void CompleteLegacyIdentity(core::TaskContract& task, const RuntimeStamp* parent = nullptr)
{
    bool migrated = false;
    if (task.stamp.taskId.empty())
    {
        task.stamp.taskId = parent ? parent->taskId : actions::NewActionId();
        migrated = true;
    }
    if (task.stamp.attemptId.empty())
    {
        task.stamp.attemptId = parent ? parent->attemptId : actions::NewActionId();
        migrated = true;
    }
    if (migrated)
        task.positiveConstraints.push_back("Host migration of legacy input fills only previously unset task and attempt identity fields");
    task.cancellation.origin = task.stamp;
}
}

void ReviaSession::InitializeTaskContracts()
{
    actionRuntime.BindTaskContracts([this](const actions::ActionRequest& request, const RuntimeStamp& stamp, std::stop_token stop)
        { return BuildActionTaskContract(request, stamp, stop); },
        [this](const core::TaskContract& task, std::stop_token stop) { return TaskContractRefusal(task, stop); });
}

std::shared_ptr<const core::TaskContract> ReviaSession::BuildTurnTaskContract(const agents::InputBatch& batch)
{
    core::TaskContract task;
    task.stamp = batch.context.stamp;
    task.scope = InputScope(batch.context);
    // Full input stays with the admitted batch; contract metadata has a smaller bound.
    task.goal = batch.text.size() <= 8192 ? batch.text
                                          : "Respond to the admitted conversation input (SHA256 " + audit::ContentDigest(batch.text) + ").";
    task.positiveConstraints = {"Respond to the accepted input within its captured participant and audience context"};
    task.negativeConstraints = {
        "Do not disclose evidence outside its captured scope", "Do not make unsupported completion or factual claims"};
    task.deliverables = {"Response or named refusal to the accepted input"};
    task.acceptanceObligations = {"Answer the accepted request with supported claims and preserve explicit uncertainty"};
    task.sourceKind = batch.source == agents::InputSource::Voice ? "voice" : "chat";
    CompleteLegacyIdentity(task);
    task.sourceId = task.stamp.taskId;
    if (!core::ValidateTaskContract(task))
        return nullptr;
    auto contract = std::make_shared<const core::TaskContract>(std::move(task));
    {
        std::lock_guard lock(taskContractMutex);
        admittedTaskInputContext = batch.context;
        foregroundTaskContract = contract;
    }
    return contract;
}

std::shared_ptr<const core::TaskContract> ReviaSession::BuildActionTaskContract(
    const actions::ActionRequest& request, const RuntimeStamp& stamp, const std::stop_token stopToken)
{
    if (stopToken.stop_requested())
        return nullptr;
    std::shared_ptr<const core::TaskContract> owner;
    std::optional<agents::InputContext> input;
    {
        std::lock_guard lock(taskContractMutex);
        if (const auto found = ownedTaskContracts.find(stamp.taskId); found != ownedTaskContracts.end())
            owner = found->second;
        else if (foregroundTaskContract && foregroundTaskContract->stamp.taskId == stamp.taskId)
            owner = foregroundTaskContract;
        input = admittedTaskInputContext;
    }
    // A named background task must carry its admission or have an exact registered owner.
    if ((!stamp.taskId.empty() && !owner) || (owner && !owner->stamp.SameSession(stamp)))
        return nullptr;
    core::TaskContract task;
    task.stamp = stamp;
    task.scope = owner ? owner->scope : input && InputContextCurrent(*input) ? InputScope(*input) : CapturePrivateMemoryScope();
    task.goal = "Execute " + actions::ToString(request.type);
    task.positiveConstraints = {"Execute only the admitted native operation under existing machine and companion authority"};
    task.negativeConstraints = {
        "Do not treat an executor receipt as proof of whole-task completion", "Do not disclose outside the captured scope"};
    task.deliverables = {"Native executor receipt or named refusal"};
    task.acceptanceObligations = {"Preserve actual attempted and succeeded executor state and required durable audit"};
    task.sourceKind = "action";
    task.sourceId = request.id;
    CompleteLegacyIdentity(task, owner ? &owner->stamp : nullptr);
    if (owner)
    {
        task.resourceCeilings = owner->resourceCeilings;
        task.positiveConstraints.insert(
            task.positiveConstraints.end(), owner->positiveConstraints.begin(), owner->positiveConstraints.end());
        task.negativeConstraints.insert(
            task.negativeConstraints.end(), owner->negativeConstraints.begin(), owner->negativeConstraints.end());
        task.cancellation = owner->cancellation;
        task.cancellation.origin.policyVersion = task.stamp.policyVersion;
    }
    if (!core::ValidateTaskContract(task) || !TaskContractRefusal(task, stopToken).empty())
        return nullptr;
    return std::make_shared<const core::TaskContract>(std::move(task));
}

std::shared_ptr<const core::TaskContract> ReviaSession::CaptureOwnedTaskContract(
    const RuntimeStamp& stamp, const std::string& goal, const std::string& parentTaskId)
{
    std::optional<agents::InputContext> input;
    std::shared_ptr<const core::TaskContract> parent;
    {
        std::lock_guard lock(taskContractMutex);
        input = admittedTaskInputContext;
        if (!parentTaskId.empty())
        {
            const auto found = ownedTaskContracts.find(parentTaskId);
            if (found == ownedTaskContracts.end() || !found->second->stamp.SameSession(stamp))
                return nullptr;
            parent = found->second;
        }
    }
    core::TaskContract task;
    if (parent)
        task = *parent;
    else
    {
        if (!input || !InputContextCurrent(*input))
            input = CaptureInputContext(agents::InputSource::Typed);
        task.scope = InputScope(*input);
        task.positiveConstraints = {"Work only within the participant, audience and capability scope captured at task admission"};
        task.negativeConstraints = {
            "Do not acquire another turn's identity, constraints or authority", "Do not disclose outside the captured scope"};
        task.deliverables = {"Evidence supporting the admitted task or a named refusal"};
        task.acceptanceObligations = {"Distinguish native execution receipts from verified whole-task completion"};
    }
    task.stamp = stamp;
    task.goal = goal.size() <= 8192 ? goal : "Admitted task (SHA256 " + audit::ContentDigest(goal) + ").";
    task.sourceKind = "task";
    CompleteLegacyIdentity(task);
    task.sourceId = task.stamp.taskId;
    if (parent)
    {
        task.cancellation = parent->cancellation;
        task.cancellation.ancestorTaskIds.push_back(parent->stamp.taskId);
        task.cancellation.origin.policyVersion = task.stamp.policyVersion;
    }
    if (!core::ValidateTaskContract(task))
        return nullptr;
    return std::make_shared<const core::TaskContract>(std::move(task));
}

bool ReviaSession::TaskContextCurrent(const core::TaskContract& task) const
{
    const agents::InputContext input{task.stamp, task.scope.audience, task.scope.participantId,
        task.scope.participantSource, task.scope.consentRevision};
    return InputContextCurrent(input);
}

std::string ReviaSession::TaskContractRefusal(const core::TaskContract& task, const std::stop_token stopToken) const
{
    if (!TaskContextCurrent(task))
        return "captured_input_context_changed";
    auto currentScope = task.scope;
    currentScope.audience = Audience();
    auto current = task.stamp;
    current.policyVersion = companionAuthority->Revision();
    const auto admitted = core::ValidateTaskAdmission(
        task, current, currentScope,
        [this](const RuntimeStamp& candidate) { return started.load() && sessionIdentity.IsCurrent(candidate); }, stopToken);
    return admitted ? std::string{} : admitted.code;
}
}
