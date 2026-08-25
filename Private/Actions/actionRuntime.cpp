#include "Actions/actionRuntime.h"

#include "Filesystem/fileSystemExecutor.h"
#include "Internet/internetSearchExecutor.h"
#include "Internet/visibleBrowserClient.h"
#include "Windows/windowsAutomationExecutor.h"

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <system_error>

namespace revia::actions
{

ActionRuntime::ActionRuntime()
    : internetCancellation(std::make_shared<internet::VisibleBrowserCancellation>())
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
        settings.minimumDesktopActionIntervalMs);
    dispatcher.Register(std::make_unique<filesystem::FileSystemExecutor>(
        settings.maxReadBytes,
        settings.maxDirectoryEntries,
        settings.maxAffectedEntries));
    dispatcher.Register(std::make_unique<internet::InternetSearchExecutor>(
        settings.internet, internetCancellation));
#ifdef _WIN32
    dispatcher.Register(std::make_unique<windows::WindowsAutomationExecutor>());
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

ActionOutcome ActionRuntime::Execute(
    const ActionRequest& request,
    bool confirmationGranted)
{
    std::lock_guard lock(mutex);
    const auto executionStarted = std::chrono::steady_clock::now();
    ActionOutcome outcome;
    outcome.policy = Evaluate(request);
    const bool otherwiseExecutable =
        outcome.policy.verdict == PolicyVerdict::Allowed ||
        (outcome.policy.verdict == PolicyVerdict::RequiresConfirmation && confirmationGranted);
    std::string rateReason;
    if (otherwiseExecutable && !desktopRateLimiter.Admit(
            request,
            std::chrono::steady_clock::now(),
            rateReason))
    {
        outcome.policy.verdict = PolicyVerdict::Blocked;
        outcome.policy.reason = rateReason;
    }
    outcome.result = dispatcher.Dispatch(request, outcome.policy, confirmationGranted);
    if (auditLogger)
    {
        const double elapsedMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - executionStarted).count();
        static_cast<void>(auditLogger->Record(
            request, outcome.policy, outcome.result, elapsedMilliseconds));
    }
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
    bool confirmationGranted)
{
    std::lock_guard lock(mutex);
    const auto executionStarted = std::chrono::steady_clock::now();
    ActionOutcome outcome;
    outcome.policy = EvaluateScoped(request, scopedPolicy);
    const bool otherwiseExecutable =
        outcome.policy.verdict == PolicyVerdict::Allowed ||
        (outcome.policy.verdict == PolicyVerdict::RequiresConfirmation && confirmationGranted);
    std::string rateReason;
    if (otherwiseExecutable && !desktopRateLimiter.Admit(
            request,
            std::chrono::steady_clock::now(),
            rateReason))
    {
        outcome.policy.verdict = PolicyVerdict::Blocked;
        outcome.policy.reason = rateReason;
    }
    outcome.result = dispatcher.Dispatch(request, outcome.policy, confirmationGranted);
    if (auditLogger)
    {
        const double elapsedMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - executionStarted).count();
        static_cast<void>(auditLogger->Record(
            request, outcome.policy, outcome.result, elapsedMilliseconds));
    }
    return outcome;
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

void ActionRuntime::CancelActiveInternet()
{
    // Deliberately do not acquire `mutex`: Execute() owns it for the full synchronous
    // request, and cancellation exists specifically to interrupt that wait.
    if (internetCancellation) internetCancellation->CancelActive();
}

} // namespace revia::actions
