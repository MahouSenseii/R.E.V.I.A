#pragma once

#include "Actions/actionDispatcher.h"
#include "Audit/actionAuditLogger.h"
#include "Planning/structuredActionParser.h"
#include "Policy/capabilityPolicy.h"
#include "Policy/permissionStore.h"

#include <filesystem>
#include <memory>
#include <string>

namespace revia::actions
{

class ActionRuntime
{
public:
    [[nodiscard]] bool Initialize(
        const std::filesystem::path& capabilityConfig,
        const std::filesystem::path& auditPath,
        std::string& outError);

    [[nodiscard]] planning::ParsedAction ParseCommand(const std::string& input) const;
    [[nodiscard]] planning::ParsedAction ParseJson(const std::string& input) const;
    [[nodiscard]] PolicyDecision Evaluate(const ActionRequest& request) const;
    [[nodiscard]] ActionOutcome Execute(const ActionRequest& request,bool confirmationGranted = false);
    [[nodiscard]] std::string StatusJson() const;
    [[nodiscard]] bool IsInitialized() const;

private:
    policy::PermissionStore permissionStore;
    std::unique_ptr<policy::CapabilityPolicy> policy;
    ActionDispatcher dispatcher;
    std::unique_ptr<audit::ActionAuditLogger> auditLogger;
    planning::StructuredActionParser parser;
};

} // namespace revia::actions
