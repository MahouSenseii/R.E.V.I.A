#include "Actions/actionRuntime.h"

#include "Filesystem/fileSystemExecutor.h"
#include "Internet/internetSearchExecutor.h"
#include "Internet/visibleBrowserClient.h"
#include "Windows/desktopControlExecutor.h"
#include "Windows/windowsAutomationExecutor.h"

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <system_error>

namespace revia::actions
{

ActionRuntime::ActionRuntime()
    : internetCancellation(std::make_shared<internet::VisibleBrowserCancellation>()),
      desktopInputGuard(std::make_shared<policy::DesktopInputGuard>())
{
}

bool ActionRuntime::Initialize(
    const std::filesystem::path& capabilityConfig,
    const std::filesystem::path& inputAuditPath,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    return InitializeUnlocked(capabilityConfig, inputAuditPath, outError);
}

bool ActionRuntime::InitializeUnlocked(
    const std::filesystem::path& capabilityConfig,
    const std::filesystem::path& inputAuditPath,
    std::string& outError)
{
    CapabilitySettings settings;
    if (!permissionStore.Load(capabilityConfig, settings, outError))
    {
        return false;
    }

    if (settings.createMissingApprovedRoots)
    {
        for (const auto& root : settings.approvedRoots)
        {
            std::error_code error;
            std::filesystem::create_directories(root, error);
            if (error)
            {
                outError = "Could not create approved root " + PathToUtf8(root) +
                    ": " + error.message();
                return false;
            }
        }
    }

    policy = std::make_unique<policy::CapabilityPolicy>(settings);
    dispatcher.Clear();
    desktopRateLimiter.Configure(
        settings.maxDesktopActionsPerMinute,
        settings.minimumDesktopActionIntervalMs,
        policy::DesktopActionRateLimiter::Scope::UiAutomation);
    desktopControlRateLimiter.Configure(
        settings.desktopControl.maxInputActionsPerMinute,
        settings.desktopControl.minimumInputIntervalMs,
        policy::DesktopActionRateLimiter::Scope::DesktopControl);
    dispatcher.Register(std::make_unique<filesystem::FileSystemExecutor>(
        settings.maxReadBytes,
        settings.maxDirectoryEntries,
        settings.maxAffectedEntries));
    dispatcher.Register(std::make_unique<internet::InternetSearchExecutor>(
        settings.internet, internetCancellation));
#ifdef _WIN32
    dispatcher.Register(std::make_unique<windows::WindowsAutomationExecutor>());
    dispatcher.Register(std::make_unique<windows::DesktopControlExecutor>(
        settings.desktopControl, desktopInputGuard));
#endif
    auditLogger = std::make_unique<audit::ActionAuditLogger>(inputAuditPath);
    capabilityConfigPath = capabilityConfig;
    auditPath = inputAuditPath;
    outError.clear();
    return true;
}

planning::ParsedAction ActionRuntime::ParseCommand(const std::string& input) const
{
    return parser.ParseCommand(input);
}

planning::ParsedAction ActionRuntime::ParseJson(const std::string& input) const
{
    return parser.ParseJson(input);
}

PolicyDecision ActionRuntime::Evaluate(const ActionRequest& request) const
{
    std::lock_guard lock(mutex);
    if (!policy)
    {
        PolicyDecision decision;
        decision.reason = "Action runtime is not initialized.";
        return decision;
    }
    return policy->Evaluate(request);
}

void ActionRuntime::SetDispatchObserver(DispatchObserver observer)
{
    std::lock_guard lock(mutex);
    dispatchObserver = std::move(observer);
}

ActionOutcome ActionRuntime::Execute(
    const ActionRequest& request,
    bool confirmationGranted,
    const std::stop_token stopToken)
{
    std::lock_guard lock(mutex);
    return ExecuteWithPolicy(request, nullptr, confirmationGranted, stopToken);
}

ActionOutcome ActionRuntime::ExecuteWithPolicy(
    const ActionRequest& request,
    const policy::CapabilityPolicy* scopedPolicy,
    const bool confirmationGranted,
    const std::stop_token stopToken)
{
    const auto executionStarted = std::chrono::steady_clock::now();
    ActionOutcome outcome;
    outcome.policy = scopedPolicy ? EvaluateScoped(request, *scopedPolicy) : Evaluate(request);
    const bool otherwiseExecutable =
        outcome.policy.verdict == PolicyVerdict::Allowed ||
        (outcome.policy.verdict == PolicyVerdict::RequiresConfirmation && confirmationGranted);
    std::string rateReason;
    if (otherwiseExecutable && !stopToken.stop_requested())
    {
        const auto admissionTime = std::chrono::steady_clock::now();
        if (!desktopRateLimiter.Admit(request, admissionTime, rateReason) ||
            !desktopControlRateLimiter.Admit(request, admissionTime, rateReason))
        {
            outcome.policy.verdict = PolicyVerdict::Blocked;
            outcome.policy.reason = rateReason;
        }
    }
    // Bracketed so the observer sees the action end on every path, including the
    // blocked and refused ones -- a scope that only closes on success is how a session
    // gets stuck in a state an aborted action put it in.
    if (dispatchObserver) dispatchObserver(request, true);
    const std::string transactionId = NewActionId();
    const bool executable = outcome.policy.verdict == PolicyVerdict::Allowed ||
        (outcome.policy.verdict == PolicyVerdict::RequiresConfirmation && confirmationGranted);
    if (stopToken.stop_requested())
    {
        outcome.result.dryRun = request.dryRun;
        outcome.result.message = "Action was cancelled before execution.";
    }
    else if (executable && (!auditLogger ||
        !auditLogger->RecordIntent(request, outcome.policy, transactionId)))
    {
        outcome.result.dryRun = request.dryRun;
        outcome.result.message = "Action was not executed because its required audit could not be written.";
        outcome.auditError = "The action intent could not be recorded durably.";
    }
    else if (stopToken.stop_requested())
    {
        // Writing the intent can take time; cancellation still wins before dispatch.
        outcome.result.dryRun = request.dryRun;
        outcome.result.message = "Action was cancelled before execution.";
    }
    else
    {
        outcome.result = dispatcher.Dispatch(request, outcome.policy, confirmationGranted);
    }
    if (dispatchObserver) dispatchObserver(request, false);
    if (auditLogger)
    {
        const double elapsedMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - executionStarted).count();
        if (!auditLogger->Record(
            request, outcome.policy, outcome.result, elapsedMilliseconds, transactionId))
        {
            if (!outcome.auditError.empty()) outcome.auditError += " ";
            outcome.auditError += "The completion audit could not be recorded; the executor result is retained.";
        }
    }
    else outcome.auditError = "The action audit is unavailable.";
    return outcome;
}

namespace
{

// Ranked least to most restrictive so two decisions can be combined without
// assuming which policy produced which verdict.
int VerdictRank(const PolicyVerdict verdict)
{
    switch (verdict)
    {
        case PolicyVerdict::Allowed: return 0;
        case PolicyVerdict::RequiresConfirmation: return 1;
        case PolicyVerdict::Blocked: return 2;
    }
    return 2;
}

PolicyDecision MoreRestrictive(const PolicyDecision& first, const PolicyDecision& second)
{
    PolicyDecision combined =
        VerdictRank(second.verdict) > VerdictRank(first.verdict) ? second : first;
    combined.risk = std::max(first.risk, second.risk);
    return combined;
}

} // namespace

PolicyDecision ActionRuntime::EvaluateScoped(
    const ActionRequest& request,
    const policy::CapabilityPolicy& scopedPolicy) const
{
    std::lock_guard lock(mutex);
    const PolicyDecision globalDecision = Evaluate(request);
    if (globalDecision.verdict == PolicyVerdict::Blocked)
    {
        return globalDecision;
    }
    return MoreRestrictive(globalDecision, scopedPolicy.Evaluate(request));
}

ActionOutcome ActionRuntime::ExecuteScoped(
    const ActionRequest& request,
    const policy::CapabilityPolicy& scopedPolicy,
    bool confirmationGranted,
    const std::stop_token stopToken)
{
    std::lock_guard lock(mutex);
    return ExecuteWithPolicy(request, &scopedPolicy, confirmationGranted, stopToken);
}

std::string ActionRuntime::StatusJson() const
{
    std::lock_guard lock(mutex);
    if (!policy)
    {
        return nlohmann::json({{"initialized", false}}).dump(2);
    }

    const auto& settings = policy->Settings();
    nlohmann::json roots = nlohmann::json::array();
    for (const auto& root : settings.approvedRoots)
    {
        roots.push_back(PathToUtf8(root));
    }
    nlohmann::json applications = settings.approvedApplications;
    nlohmann::json controls = settings.approvedControls;
    return nlohmann::json({
        {"initialized", true},
        {"mode", ToString(settings.mode)},
        {"approved_roots", roots},
        {"approved_applications", applications},
        {"approved_controls", controls},
        {"auto_approve_risk_through", ToString(settings.autoApproveRiskThrough)},
        {"max_read_bytes", settings.maxReadBytes},
        {"max_directory_entries", settings.maxDirectoryEntries},
        {"max_affected_entries", settings.maxAffectedEntries},
        {"max_desktop_actions_per_minute", settings.maxDesktopActionsPerMinute},
        {"minimum_desktop_action_interval_ms", settings.minimumDesktopActionIntervalMs},
        {"desktop_control", {
            {"pointer", settings.desktopControl.pointer},
            {"keyboard", settings.desktopControl.keyboard},
            {"application_launch", settings.desktopControl.applicationLaunch},
            {"raw_coordinates", settings.desktopControl.rawCoordinates},
            {"autonomous", settings.desktopControl.autonomous},
            {"scope", ToString(settings.desktopControl.scope)},
            {"allow_command_surfaces", settings.desktopControl.allowCommandSurfaces},
            {"max_input_actions_per_minute",
                settings.desktopControl.maxInputActionsPerMinute},
            {"minimum_input_interval_ms",
                settings.desktopControl.minimumInputIntervalMs},
            {"max_typed_characters", settings.desktopControl.maxTypedCharacters},
            {"stopped", desktopInputGuard && desktopInputGuard->IsTripped()},
            {"stop_reason", desktopInputGuard ? desktopInputGuard->Reason() : std::string{}}
        }},
        {"internet", {
            {"enabled", settings.internet.enabled},
            {"automatic_lookup", settings.internet.automaticLookup},
            {"provider", settings.internet.provider},
            {"approved_hosts", settings.internet.approvedHosts},
            {"request_timeout_ms", settings.internet.requestTimeoutMs},
            {"max_response_bytes", settings.internet.maxResponseBytes},
            {"max_requests_per_minute", settings.internet.maxRequestsPerMinute},
            {"max_results", settings.internet.maxResults},
            {"visible_browser", settings.internet.visibleBrowser},
            {"autonomous_research", settings.internet.autonomousResearch}
        }}
    }).dump(2);
}

bool ActionRuntime::IsInitialized() const
{
    std::lock_guard lock(mutex);
    return policy != nullptr;
}

CapabilitySettings ActionRuntime::Settings() const
{
    std::lock_guard lock(mutex);
    return policy != nullptr ? policy->Settings() : CapabilitySettings{};
}

bool ActionRuntime::ReloadUnlocked(std::string& outError)
{
    if (capabilityConfigPath.empty() || auditPath.empty())
    {
        outError = "Action runtime has no capability configuration to reload.";
        return false;
    }
    return InitializeUnlocked(capabilityConfigPath, auditPath, outError);
}

bool ActionRuntime::AddApprovedApplication(
    const std::string& executable,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    return capabilityEditor.AddApplication(capabilityConfigPath, executable, outError) &&
        ReloadUnlocked(outError);
}

bool ActionRuntime::RemoveApprovedApplication(
    const std::string& executable,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    return capabilityEditor.RemoveApplication(capabilityConfigPath, executable, outError) &&
        ReloadUnlocked(outError);
}

bool ActionRuntime::AddApprovedControl(
    const std::string& executable,
    const std::string& control,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    return capabilityEditor.AddControl(
            capabilityConfigPath, executable, control, outError) &&
        ReloadUnlocked(outError);
}

bool ActionRuntime::RemoveApprovedControl(
    const std::string& executable,
    const std::string& control,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    return capabilityEditor.RemoveControl(
            capabilityConfigPath, executable, control, outError) &&
        ReloadUnlocked(outError);
}

bool ActionRuntime::SetInternetAccess(
    const bool enabled,
    const bool automaticLookup,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    return capabilityEditor.SetInternetAccess(
            capabilityConfigPath, enabled, automaticLookup, outError) &&
        ReloadUnlocked(outError);
}

bool ActionRuntime::SetInternetBrowser(
    const bool visibleBrowser,
    const bool autonomousResearch,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    return capabilityEditor.SetInternetBrowser(
            capabilityConfigPath, visibleBrowser, autonomousResearch, outError) &&
        ReloadUnlocked(outError);
}

bool ActionRuntime::SetCameraAccess(
    const bool enabled,
    const bool autonomousCapture,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    return capabilityEditor.SetCameraAccess(
            capabilityConfigPath, enabled, autonomousCapture, outError) &&
        ReloadUnlocked(outError);
}

bool ActionRuntime::SetDesktopControl(
    const bool pointer,
    const bool keyboard,
    const bool applicationLaunch,
    const bool rawCoordinates,
    const bool autonomous,
    const CapabilitySettings::DesktopControl::InputScope scope,
    const bool allowCommandSurfaces,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    return capabilityEditor.SetDesktopControl(
            capabilityConfigPath, pointer, keyboard, applicationLaunch, rawCoordinates,
            autonomous, scope, allowCommandSurfaces, outError) &&
        ReloadUnlocked(outError);
}

bool ActionRuntime::SetExecutionMode(const ExecutionMode mode, std::string& outError)
{
    std::lock_guard lock(mutex);
    return capabilityEditor.SetExecutionMode(capabilityConfigPath, mode, outError) &&
        ReloadUnlocked(outError);
}

void ActionRuntime::StopDesktopControl(const std::string& reason)
{
    // Deliberately does not acquire `mutex`: Execute() owns it for the whole action,
    // and an emergency stop that waits for the action it is stopping is not one.
    if (desktopInputGuard) desktopInputGuard->Trip(reason);
}

bool ActionRuntime::ResumeDesktopControl()
{
    return desktopInputGuard && desktopInputGuard->Resume();
}

bool ActionRuntime::DesktopControlStopped() const
{
    return desktopInputGuard && desktopInputGuard->IsTripped();
}

std::string ActionRuntime::DesktopControlStopReason() const
{
    return desktopInputGuard ? desktopInputGuard->Reason() : std::string{};
}

void ActionRuntime::CancelActiveInternet()
{
    // Deliberately do not acquire `mutex`: Execute() owns it for the full synchronous
    // request, and cancellation exists specifically to interrupt that wait.
    if (internetCancellation) internetCancellation->CancelActive();
}

} // namespace revia::actions
