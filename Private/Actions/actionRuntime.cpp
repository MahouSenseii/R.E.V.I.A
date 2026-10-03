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
#include <utility>

namespace revia::actions
{

ActionRuntime::ActionRuntime()
    : internetCancellation(std::make_shared<internet::VisibleBrowserCancellation>()),
      desktopInputGuard(std::make_shared<policy::DesktopInputGuard>())
{
}

bool ActionRuntime::Initialize(const std::filesystem::path& capabilityConfig,
    const std::filesystem::path& inputAuditPath, std::string& outError)
{
    std::lock_guard lock(mutex);
    return InitializeUnlocked(capabilityConfig, inputAuditPath, outError);
}

bool ActionRuntime::InitializeUnlocked(const std::filesystem::path& capabilityConfig,
    const std::filesystem::path& inputAuditPath, std::string& outError)
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

    settings.internet.profileDirectory = browserProfileDirectory;
    settings.internet.logDirectory = browserLogDirectory;
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
    dispatcher.Register(std::make_unique<windows::WindowsAutomationExecutor>(
        settings.desktopControl, desktopApprovals));
    dispatcher.Register(std::make_unique<windows::DesktopControlExecutor>(
        settings.desktopControl, desktopInputGuard, desktopApprovals));
#endif
    auditLogger = std::make_unique<audit::ActionAuditLogger>(inputAuditPath);
    capabilityConfigPath = capabilityConfig;
    auditPath = inputAuditPath;
    {
        std::lock_guard settingsLock(settingsMutex);
        settingsSnapshot = std::move(settings);
    }

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
    return policy::CapabilityPolicy(Settings()).Evaluate(request);
}

void ActionRuntime::SetDispatchObserver(DispatchObserver observer)
{
    std::lock_guard lock(mutex);
    dispatchObserver = std::move(observer);
}

ActionOutcome ActionRuntime::Execute(const ActionRequest& request, bool confirmationGranted, const std::stop_token stopToken)
{
    std::lock_guard lock(mutex);
    return ExecuteWithPolicy(request, nullptr, confirmationGranted, stopToken);
}

void ActionRuntime::BindAuthority(std::shared_ptr<policy::CompanionAuthority> inputAuthority, runtime::RuntimeStamp inputStamp)
{
    std::lock_guard lock(mutex);
    authority = std::move(inputAuthority);
    sessionStamp = std::move(inputStamp);
}

void ActionRuntime::ClearAuthorityBinding()
{
    std::lock_guard lock(mutex);
    authority.reset();
    sessionStamp = {};
}

ActionOutcome ActionRuntime::ExecuteFor(
    const runtime::RuntimeStamp& stamp, const ActionRequest& request, const bool confirmationGranted, const std::stop_token stopToken)
{
    std::lock_guard lock(mutex);
    return ExecuteWithPolicy(request, nullptr, confirmationGranted, stopToken, &stamp);
}

ActionOutcome ActionRuntime::ExecuteScopedFor(const runtime::RuntimeStamp& stamp, const ActionRequest& request,
    const policy::CapabilityPolicy& scopedPolicy, const bool confirmationGranted, const std::stop_token stopToken)
{
    std::lock_guard lock(mutex);
    return ExecuteWithPolicy(request, &scopedPolicy, confirmationGranted, stopToken, &stamp);
}

void ActionRuntime::SetPrivateRuntimePaths(std::filesystem::path profileDirectory, std::filesystem::path logDirectory)
{
    std::lock_guard lock(mutex);
    browserProfileDirectory = std::move(profileDirectory);
    browserLogDirectory = std::move(logDirectory);
}

ActionOutcome ActionRuntime::ExecuteWithPolicy(const ActionRequest& inputRequest, const policy::CapabilityPolicy* scopedPolicy,
    const bool confirmationGranted, const std::stop_token stopToken, const runtime::RuntimeStamp* subject)
{
    const auto executionStarted = std::chrono::steady_clock::now();
    ActionRequest request = inputRequest;
    request.authorityStamp = {};
    ActionOutcome outcome;
    outcome.policy = scopedPolicy ? EvaluateScoped(request, *scopedPolicy) : Evaluate(request);
    const auto capturedAuthority = authority;
    const auto capturedStamp = subject ? *subject : sessionStamp;
    if (capturedAuthority)
    {
        request.authorityStamp = capturedStamp;
        request.authorityStamp.policyVersion = capturedAuthority->Revision();
    }
    const auto applyAuthority = [&](PolicyDecision decision)
    {
        if (subject && (!capturedAuthority || !capturedStamp.SameSession(sessionStamp)))
        {
            decision.verdict = PolicyVerdict::Blocked;
            decision.reason = "The action subject does not match the runtime authority binding.";
        }
        else if (capturedAuthority && decision.verdict != PolicyVerdict::Blocked)
        {
            const auto refusal = capturedAuthority->Evaluate(capturedStamp, request, decision);
            if (!refusal.empty())
            {
                decision.verdict = PolicyVerdict::Blocked;
                decision.reason = refusal;
            }
        }
        return decision;
    };
    outcome.policy = applyAuthority(std::move(outcome.policy));
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
    if (outcome.policy.verdict != PolicyVerdict::Blocked)
        outcome.policy = applyAuthority(scopedPolicy ? EvaluateScoped(request, *scopedPolicy) : Evaluate(request));
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
        if (outcome.policy.verdict != PolicyVerdict::Blocked)
            outcome.policy = applyAuthority(scopedPolicy ? EvaluateScoped(request, *scopedPolicy) : Evaluate(request));
        ActionRequest guardedRequest = request;
        const auto priorEffect = request.beforeEffect;
        const auto canonicalDecision = outcome.policy;
        guardedRequest.beforeEffect = [this, request, priorEffect, capturedAuthority, capturedStamp, stopToken, canonicalDecision,
                                          scopedPolicy, confirmationGranted](const std::string& resource)
        {
            if (stopToken.stop_requested())
                return std::string("Action cancellation was requested.");
            if (priorEffect)
            {
                const auto refusal = priorEffect(resource);
                if (!refusal.empty())
                    return refusal;
            }
            if (stopToken.stop_requested())
                return std::string("Action cancellation was requested.");
            const auto current = scopedPolicy ? EvaluateScoped(request, *scopedPolicy) : Evaluate(request);
            if (current.verdict == PolicyVerdict::Blocked ||
                (current.verdict == PolicyVerdict::RequiresConfirmation && !confirmationGranted))
                return current.reason;
            if (current.canonicalSource != canonicalDecision.canonicalSource ||
                current.canonicalDestination != canonicalDecision.canonicalDestination)
                return std::string("The canonical action resource changed before its effect.");
            if (capturedAuthority && (authority != capturedAuthority || !sessionStamp.SameSession(capturedStamp)))
                return std::string("The runtime authority binding changed before its effect.");
            if (request.type == ActionType::WebSearch && !resource.empty())
            {
                const auto currentInternet = Settings().internet;
                if (resource == "visible_browser" && !currentInternet.visibleBrowser)
                    return std::string("The visible browser authority was withdrawn.");
                if (resource != "visible_browser" && std::find(currentInternet.approvedHosts.begin(), currentInternet.approvedHosts.end(),
                                                         resource) == currentInternet.approvedHosts.end())
                    return std::string("The network host is outside the current machine ceiling.");
            }
            if (stopToken.stop_requested())
                return std::string("Action cancellation was requested.");
            return capturedAuthority ? capturedAuthority->Evaluate(capturedStamp, request, current, resource) : std::string{};
        };
        outcome.result = dispatcher.Dispatch(guardedRequest, outcome.policy, confirmationGranted);
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

PolicyDecision ActionRuntime::EvaluateScoped(const ActionRequest& request, const policy::CapabilityPolicy& scopedPolicy) const
{
    std::lock_guard lock(mutex);
    const PolicyDecision globalDecision = Evaluate(request);
    if (globalDecision.verdict == PolicyVerdict::Blocked)
    {
        return globalDecision;
    }
    return MoreRestrictive(globalDecision, scopedPolicy.Evaluate(request));
}

ActionOutcome ActionRuntime::ExecuteScoped(const ActionRequest& request,
    const policy::CapabilityPolicy& scopedPolicy, bool confirmationGranted, const std::stop_token stopToken)
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
            {"visual_targeting", settings.desktopControl.visualTargeting},
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

void ActionRuntime::CancelActiveInternet(const bool preserveTaskOwned)
{
    // Deliberately do not acquire `mutex`: Execute() owns it for the full synchronous
    // request, and cancellation exists specifically to interrupt that wait.
    if (internetCancellation) internetCancellation->CancelActive(preserveTaskOwned);
}

} // namespace revia::actions
