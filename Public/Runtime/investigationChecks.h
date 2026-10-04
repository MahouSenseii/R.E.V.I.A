#pragma once

#include "Agents/investigationAgent.h"
#include "Runtime/runtimeStamp.h"

#include <functional>
#include <stop_token>
#include <string>

namespace revia::actions
{
class ActionRuntime;
}

namespace revia::runtime
{

[[nodiscard]] agents::ExecutedCheck ExecuteInvestigationCheck(actions::ActionRuntime& runtime, const RuntimeStamp& origin,
    agents::CheckKind kind, const std::string& proposalJson, std::stop_token stopToken, const std::function<bool()>& admission);

}
