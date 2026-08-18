#include "Actions/actionRuntime.h"

#include "Filesystem/fileSystemExecutor.h"
#include "Windows/windowsAutomationExecutor.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <system_error>

namespace revia::actions
{

bool ActionRuntime::Initialize(
    const std::filesystem::path& capabilityConfig,
    const std::filesystem::path& auditPath,
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
    dispatcher.Register(std::make_unique<filesystem::FileSystemExecutor>(
        settings.maxReadBytes,
        settings.maxDirectoryEntries,
        settings.maxAffectedEntries));
#ifdef _WIN32
    dispatcher.Register(std::make_unique<windows::WindowsAutomationExecutor>());
#endif
    auditLogger = std::make_unique<audit::ActionAuditLogger>(auditPath);
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
    ActionOutcome outcome;
    outcome.policy = Evaluate(request);
    outcome.result = dispatcher.Dispatch(request, outcome.policy, confirmationGranted);
    if (auditLogger)
    {
        static_cast<void>(auditLogger->Record(request, outcome.policy, outcome.result));
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
    ActionOutcome outcome;
    outcome.policy = EvaluateScoped(request, scopedPolicy);
    outcome.result = dispatcher.Dispatch(request, outcome.policy, confirmationGranted);
    if (auditLogger)
    {
        static_cast<void>(auditLogger->Record(request, outcome.policy, outcome.result));
    }
    return outcome;
}

std::string ActionRuntime::StatusJson() const
{
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
    return nlohmann::json({
        {"initialized", true},
        {"mode", ToString(settings.mode)},
        {"approved_roots", roots},
        {"approved_applications", applications},
        {"auto_approve_risk_through", ToString(settings.autoApproveRiskThrough)},
        {"max_read_bytes", settings.maxReadBytes},
        {"max_directory_entries", settings.maxDirectoryEntries},
        {"max_affected_entries", settings.maxAffectedEntries}
    }).dump(2);
}

bool ActionRuntime::IsInitialized() const
{
    return policy != nullptr;
}

} // namespace revia::actions
