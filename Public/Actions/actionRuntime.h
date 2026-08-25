#pragma once

#include "Actions/actionDispatcher.h"
#include "Audit/actionAuditLogger.h"
#include "Planning/structuredActionParser.h"
#include "Policy/capabilityEditor.h"
#include "Policy/capabilityPolicy.h"
#include "Policy/desktopActionRateLimiter.h"
#include "Policy/permissionStore.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace revia::actions
{

namespace internet
{
class VisibleBrowserCancellation;
}

class ActionRuntime
{
public:
    ActionRuntime();

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
    [[nodiscard]] bool AddApprovedApplication(
        const std::string& executable, std::string& outError);
    [[nodiscard]] bool RemoveApprovedApplication(
        const std::string& executable, std::string& outError);
    [[nodiscard]] bool AddApprovedControl(
        const std::string& executable,
        const std::string& control,
        std::string& outError);
    [[nodiscard]] bool RemoveApprovedControl(
        const std::string& executable,
        const std::string& control,
        std::string& outError);
    [[nodiscard]] bool SetInternetAccess(
        bool enabled,
        bool automaticLookup,
        std::string& outError);
    [[nodiscard]] bool SetInternetBrowser(
        bool visibleBrowser,
        bool autonomousResearch,
        std::string& outError);
    // Lock-free with respect to Execute(): shutdown must be able to interrupt a browser
    // request while that request owns the main action-runtime mutex.
    void CancelActiveInternet();

private:
    [[nodiscard]] bool InitializeUnlocked(
        const std::filesystem::path& capabilityConfig,
        const std::filesystem::path& auditPath,
        std::string& outError);
    [[nodiscard]] bool ReloadUnlocked(std::string& outError);

    mutable std::recursive_mutex mutex;
    policy::PermissionStore permissionStore;
    policy::CapabilityEditor capabilityEditor;
    std::unique_ptr<policy::CapabilityPolicy> policy;
    std::shared_ptr<internet::VisibleBrowserCancellation> internetCancellation;
    ActionDispatcher dispatcher;
    std::unique_ptr<audit::ActionAuditLogger> auditLogger;
    planning::StructuredActionParser parser;
    policy::DesktopActionRateLimiter desktopRateLimiter;
    std::filesystem::path capabilityConfigPath;
    std::filesystem::path auditPath;
};

} // namespace revia::actions
