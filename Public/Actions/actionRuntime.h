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

    // Scoped evaluation for the goal runner. A goal carries its own, narrower
    // CapabilitySettings; the result is the more restrictive of the global
    // policy and that scoped policy, so a goal can only ever lose authority,
    // never gain it. Dispatch and audit stay on the existing path.
    [[nodiscard]] PolicyDecision EvaluateScoped(
        const ActionRequest& request,
        const policy::CapabilityPolicy& scopedPolicy) const;

    [[nodiscard]] ActionOutcome ExecuteScoped(
        const ActionRequest& request,
        const policy::CapabilityPolicy& scopedPolicy,
        bool confirmationGranted = false);
    [[nodiscard]] std::string StatusJson() const;
    [[nodiscard]] bool IsInitialized() const;

    // The configured settings, so a caller can derive a narrower scope from them. Returns
    // defaults when uninitialized, which are the most restrictive values in the struct.
    [[nodiscard]] CapabilitySettings Settings() const;

private:
    policy::PermissionStore permissionStore;
    std::unique_ptr<policy::CapabilityPolicy> policy;
    ActionDispatcher dispatcher;
    std::unique_ptr<audit::ActionAuditLogger> auditLogger;
    planning::StructuredActionParser parser;
};

} // namespace revia::actions
