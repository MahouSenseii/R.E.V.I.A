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
    std::shared_ptr<const core::TaskContract> foreground;
    std::optional<agents::InputContext> input;
    {
        std::lock_guard lock(taskContractMutex);
        foreground = foregroundTaskContract;
        input = admittedTaskInputContext;
    }
    core::TaskContract task;
    task.stamp = stamp;
    task.scope = input && InputContextCurrent(*input) ? InputScope(*input) : CapturePrivateMemoryScope();
    task.goal = "Execute " + actions::ToString(request.type);
    task.positiveConstraints = {"Execute only the admitted native operation under existing machine and companion authority"};
    task.negativeConstraints = {
        "Do not treat an executor receipt as proof of whole-task completion", "Do not disclose outside the captured scope"};
    task.deliverables = {"Native executor receipt or named refusal"};
    task.acceptanceObligations = {"Preserve actual attempted and succeeded executor state and required durable audit"};
    task.sourceKind = "action";
    task.sourceId = request.id;
    const bool sameForeground = foreground && foreground->stamp.SameSession(stamp) &&
                                foreground->stamp.policyVersion == stamp.policyVersion &&
                                core::SameMemoryScope(foreground->scope, task.scope);
    CompleteLegacyIdentity(task, sameForeground ? &foreground->stamp : nullptr);
    if (sameForeground)
    {
        task.resourceCeilings = foreground->resourceCeilings;
        task.positiveConstraints.insert(
            task.positiveConstraints.end(), foreground->positiveConstraints.begin(), foreground->positiveConstraints.end());
        task.negativeConstraints.insert(
            task.negativeConstraints.end(), foreground->negativeConstraints.begin(), foreground->negativeConstraints.end());
        task.cancellation = foreground->cancellation;
        if (task.stamp.taskId != foreground->stamp.taskId)
            task.cancellation.ancestorTaskIds.push_back(foreground->stamp.taskId);
    }
    if (!core::ValidateTaskContract(task) || !TaskContractRefusal(task, stopToken).empty())
        return nullptr;
    return std::make_shared<const core::TaskContract>(std::move(task));
}

std::string ReviaSession::TaskContractRefusal(const core::TaskContract& task, const std::stop_token stopToken) const
{
    std::optional<agents::InputContext> input;
    {
        std::lock_guard lock(taskContractMutex);
        input = admittedTaskInputContext;
    }
    memory::MemoryScope currentScope;
    if (input && core::SameMemoryScope(task.scope, InputScope(*input)))
    {
        if (!InputContextCurrent(*input))
            return "captured_input_context_changed";
        currentScope = InputScope(*input);
    }
    else
        currentScope = CapturePrivateMemoryScope();
    auto current = task.stamp;
    current.policyVersion = companionAuthority->Revision();
    const auto admitted = core::ValidateTaskAdmission(
        task, current, currentScope,
        [this](const RuntimeStamp& candidate) { return started.load() && sessionIdentity.IsCurrent(candidate); }, stopToken);
    return admitted ? std::string{} : admitted.code;
}
}
