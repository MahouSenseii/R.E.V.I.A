#pragma once

#include "Actions/actionDispatcher.h"
#include "Audit/actionAuditLogger.h"
#include "Planning/structuredActionParser.h"
#include "Policy/capabilityEditor.h"
#include "Policy/capabilityPolicy.h"
#include "Policy/desktopActionRateLimiter.h"
#include "Policy/desktopInputGuard.h"
#include "Policy/permissionStore.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
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
    [[nodiscard]] ActionOutcome Execute(
        const ActionRequest& request,
        bool confirmationGranted = false,
        std::stop_token stopToken = {});

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
        bool confirmationGranted = false,
        std::stop_token stopToken = {});
    // Notified immediately before and immediately after every dispatched action,
    // whichever entry point reached it.
    //
    // Exists so a caller can scope runtime state to the life of an action without every
    // call site having to remember to. Both Execute and ExecuteScoped funnel through the
    // same dispatch, so one observer covers the command path, the LLM-planned path, the
    // vision-resolved path, and goal steps -- which is the difference between a policy
    // that is wired and one that is merely declared.
    //
    // The observer is called with the dispatcher lock held. It must not execute another
    // action or block on model or network work.
    using DispatchObserver = std::function<void(const ActionRequest&, bool beginning)>;
    void SetDispatchObserver(DispatchObserver observer);

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
    [[nodiscard]] bool SetCameraAccess(
        bool enabled,
        bool autonomousCapture,
        std::string& outError);
    // pointer/keyboard/applicationLaunch are the hands themselves; rawCoordinates and
    // autonomous are narrower authorities inside them and are dropped when the
    // authority they are a subset of is withdrawn.
    [[nodiscard]] bool SetDesktopControl(
        bool pointer,
        bool keyboard,
        bool applicationLaunch,
        bool rawCoordinates,
        bool autonomous,
        CapabilitySettings::DesktopControl::InputScope scope,
        bool allowCommandSurfaces,
        std::string& outError);
    [[nodiscard]] bool SetExecutionMode(ExecutionMode mode, std::string& outError);

    // The emergency stop for synthesized input. Deliberately reachable without the
    // action mutex and without a model turn: it exists for the case where Revia is
    // mid-action and the answer has to be "stop now", not "stop when you get to it".
    void StopDesktopControl(const std::string& reason);
    // Returns whether desktop control had actually been stopped.
    bool ResumeDesktopControl();
    [[nodiscard]] bool DesktopControlStopped() const;
    [[nodiscard]] std::string DesktopControlStopReason() const;
    // Lock-free with respect to Execute(): shutdown must be able to interrupt a browser
    // request while that request owns the main action-runtime mutex.
    void CancelActiveInternet();

private:
    // Called under mutex. Cancellation is checked after the observer callback,
    // immediately before dispatch; completed executor results are never rewritten.
    [[nodiscard]] ActionOutcome ExecuteWithPolicy(
        const ActionRequest& request,
        const policy::CapabilityPolicy* scopedPolicy,
        bool confirmationGranted,
        std::stop_token stopToken);

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
    DispatchObserver dispatchObserver;
    policy::DesktopActionRateLimiter desktopRateLimiter;
    policy::DesktopActionRateLimiter desktopControlRateLimiter;
    std::shared_ptr<policy::DesktopInputGuard> desktopInputGuard;
    std::filesystem::path capabilityConfigPath;
    std::filesystem::path auditPath;
};

} // namespace revia::actions
