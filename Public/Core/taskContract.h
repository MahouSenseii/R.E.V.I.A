#pragma once

#include "Agents/agentWorkflow.h"
#include "Agents/agentDeliverable.h"
#include "Memory/memoryScope.h"
#include "Runtime/runtimeStamp.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace revia::runtime
{
struct TurnContext;
}
namespace revia::actions
{
struct ActionRequest;
}

namespace revia::core
{
struct SchemaVersion
{
    std::uint32_t major = 1;
    std::uint32_t minor = 0;
};

struct ContractValidation
{
    bool valid = false;
    std::string code;
    std::string field;
    std::string message;
    explicit operator bool() const
    {
        return valid;
    }
};

struct CancellationLineage
{
    runtime::RuntimeStamp origin;
    std::vector<std::string> ancestorTaskIds;
};

struct TaskContract
{
    SchemaVersion version;
    runtime::RuntimeStamp stamp;
    memory::MemoryScope scope;
    std::string goal;
    std::vector<std::string> positiveConstraints;
    std::vector<std::string> negativeConstraints;
    std::vector<std::string> deliverables;
    std::vector<std::string> acceptanceObligations;
    agents::WorkflowBudget resourceCeilings;
    agents::DeliverableContract deliverableContract;
    CancellationLineage cancellation;
    std::string sourceKind;
    std::string sourceId;
};

using CurrentStampGuard = std::function<bool(const runtime::RuntimeStamp&)>;
inline constexpr std::size_t MaximumContractJsonBytes = 262144;

[[nodiscard]] bool SameRuntimeStamp(const runtime::RuntimeStamp& left, const runtime::RuntimeStamp& right);
[[nodiscard]] bool SameMemoryScope(const memory::MemoryScope& left, const memory::MemoryScope& right);
[[nodiscard]] ContractValidation ValidateTaskContract(const TaskContract& contract);
[[nodiscard]] ContractValidation ValidateTaskAdmission(const TaskContract& contract, const runtime::RuntimeStamp& current,
    const memory::MemoryScope& scope, const CurrentStampGuard& matchesCurrent, std::stop_token cancellation = {});
[[nodiscard]] ContractValidation SerializeTaskContract(const TaskContract& contract, std::string& output);
[[nodiscard]] ContractValidation DeserializeTaskContract(std::string_view json, TaskContract& output);

// The host supplies obligations and scope; these adapters never generate an identity or grant.
[[nodiscard]] ContractValidation AdaptTurnTask(const runtime::TurnContext& turn, TaskContract requirements, TaskContract& output);
[[nodiscard]] ContractValidation AdaptNodeTask(const agents::NodeRequest& node, TaskContract requirements, TaskContract& output);
[[nodiscard]] ContractValidation AdaptActionTask(const actions::ActionRequest& action, TaskContract requirements, TaskContract& output);
}
