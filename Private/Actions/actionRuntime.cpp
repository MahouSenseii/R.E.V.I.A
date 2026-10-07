#include "Actions/actionRuntime.h"

#include "Core/taskContract.h"
#include "Filesystem/fileSystemExecutor.h"
#include "Process/processExecutor.h"
#include "Browser/browserExecutor.h"
#include "Internet/internetSearchExecutor.h"
#include "Internet/visibleBrowserClient.h"
#include "Windows/desktopControlExecutor.h"
#include "Windows/windowsAutomationExecutor.h"
#include "Visual/imageExecutor.h"

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
    settings.image = imageAccess;
    policy = std::make_unique<policy::CapabilityPolicy>(settings);
    dispatcher.Clear();
    dispatcher.Register(std::make_unique<process::ProcessExecutor>());
    dispatcher.Register(std::make_unique<browser::BrowserExecutor>(browserSession));
    if (imageProvider)
        dispatcher.Register(std::make_unique<visual::ImageExecutor>(*imageProvider, imageAccess.outputRoot));
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
    if (!auditLogger || auditLogger->Path() != inputAuditPath)
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
    if (browserSession) browserSession->Stop();
    authority.reset();
    sessionStamp = {};
    taskContractFactory = {};
    taskContractGuard = {};
    ++taskContractBindingRevision;
}

void ActionRuntime::BindTaskContracts(TaskContractFactory factory, TaskContractGuard guard)
{
    std::lock_guard lock(mutex);
    taskContractFactory = std::move(factory);
    taskContractGuard = std::move(guard);
    ++taskContractBindingRevision;
}

std::shared_ptr<audit::EvidenceJournal> ActionRuntime::EvidenceJournalOwner() const
{
    std::lock_guard lock(mutex);
    return auditLogger ? auditLogger->Journal() : nullptr;
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

void ActionRuntime::SetBrowserSession(std::shared_ptr<browser::BrowserSession> session)
{
    std::lock_guard lock(mutex);
    if (browserSession) browserSession->Stop();
    browserSession = std::move(session);
    dispatcher.Unregister(ActionType::BrowserNavigate);
    dispatcher.Register(std::make_unique<browser::BrowserExecutor>(browserSession));
}

void ActionRuntime::BindImageProvider(visual::ImageGenerator& provider, const std::filesystem::path& ownedOutputRoot,
    const bool allowAutonomous)
{
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(ownedOutputRoot, error);
    const bool valid = !error && ownedOutputRoot.is_absolute() && root ==
        std::filesystem::weakly_canonical(provider.OutputDirectory(), error) && !error;
    std::lock_guard lock(mutex);
    imageProvider = &provider;
    imageAccess = {valid && provider.IsEnabled(), allowAutonomous, valid ? root : std::filesystem::path{}};
    dispatcher.Unregister(ActionType::GenerateImage);
    dispatcher.Register(std::make_unique<visual::ImageExecutor>(provider, imageAccess.outputRoot));
    {
        std::lock_guard settingsLock(settingsMutex);
        settingsSnapshot.image = imageAccess;
    }
    if (policy)
        policy = std::make_unique<policy::CapabilityPolicy>(Settings());
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
    const auto capturedContractGuard = taskContractGuard;
    const auto capturedContractBinding = taskContractBindingRevision;
    std::shared_ptr<const core::TaskContract> hostContract;
    std::string contractError;
    try
    {
        if (taskContractFactory && !request.taskContract)
            hostContract = taskContractFactory(request, request.authorityStamp, stopToken);
        if (!request.taskContract)
            request.taskContract = hostContract;
        if (taskContractFactory && !request.taskContract)
            contractError = "The host could not construct the action task contract.";
    }
    catch (...)
    {
        contractError = "The host task contract factory failed.";
    }
    const auto contractRefusal = [this, request, hostContract, contractError, capturedStamp, capturedContractGuard, capturedContractBinding,
                                     stopToken]() -> std::string
    {
        if (!contractError.empty())
            return contractError;
        if (!request.taskContract)
            return capturedContractGuard ? "The bound host admission requires a task contract." : std::string{};
        if (taskContractBindingRevision != capturedContractBinding || !capturedContractGuard)
            return "Task contract admission requires the originating host guard.";
        const auto& task = *request.taskContract;
        const auto shape = core::ValidateTaskContract(task);
        if (!shape)
            return "Task contract refused: " + shape.code;
        if (task.sourceKind != "action" || task.sourceId != request.id)
            return "Task contract action provenance does not match the request.";
        if ((!capturedStamp.companionId.empty() && !task.stamp.SameSession(capturedStamp)) ||
            (!capturedStamp.taskId.empty() && task.stamp.taskId != capturedStamp.taskId) ||
            (!capturedStamp.attemptId.empty() && task.stamp.attemptId != capturedStamp.attemptId) ||
            (capturedStamp.policyVersion != 0 && task.stamp.policyVersion != capturedStamp.policyVersion))
            return "Task contract does not retain the admitted authority subject.";
        if (hostContract &&
            (!core::SameRuntimeStamp(task.stamp, hostContract->stamp) || !core::SameMemoryScope(task.scope, hostContract->scope)))
            return "Task contract differs from the host captured identity or scope.";
        try
        {
            const auto refusal = capturedContractGuard(task, stopToken);
            return refusal.empty() ? std::string{} : "Task contract refused: " + refusal;
        }
        catch (...)
        {
            return "The host task contract guard failed.";
        }
    };
    // Metadata identities never replace the subject evaluated by CompanionAuthority.
    const auto applyContract = [&](PolicyDecision decision)
    {
        const auto refusal = contractRefusal();
        if (!refusal.empty())
        {
            decision.verdict = PolicyVerdict::Blocked;
            decision.reason = refusal;
        }
        return decision;
    };
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
    outcome.policy = applyContract(applyAuthority(std::move(outcome.policy)));
    const bool contractAdmitted = contractRefusal().empty();
    if (request.taskContract && contractAdmitted)
    {
        request.authorityStamp = request.taskContract->stamp;
        if (auditLogger)
            auditLogger->SetScope(request.taskContract->scope);
    }
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
        outcome.policy = applyContract(applyAuthority(scopedPolicy ? EvaluateScoped(request, *scopedPolicy) : Evaluate(request)));
    if (outcome.policy.verdict != PolicyVerdict::Blocked && request.taskContract && auditLogger &&
        auditLogger->Journal()->HasUnresolvedForTask(request.taskContract->stamp, request.taskContract->scope))
    {
        outcome.policy.verdict = PolicyVerdict::Blocked;
        outcome.policy.reason = "An unresolved action intent prevents dependent effects for this task.";
    }
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
            outcome.policy = applyContract(applyAuthority(scopedPolicy ? EvaluateScoped(request, *scopedPolicy) : Evaluate(request)));
        ActionRequest guardedRequest = request;
        const auto priorEffect = request.beforeEffect;
        const auto canonicalDecision = outcome.policy;
        guardedRequest.beforeEffect = [this, request, priorEffect, capturedAuthority, capturedStamp, stopToken, canonicalDecision,
                                          scopedPolicy, confirmationGranted, contractRefusal](const std::string& resource)
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
            const auto taskRefusal = contractRefusal();
            if (!taskRefusal.empty()) return taskRefusal;
            const auto current = scopedPolicy ? EvaluateScoped(request, *scopedPolicy) : Evaluate(request);
            if (current.verdict == PolicyVerdict::Blocked ||
                (current.verdict == PolicyVerdict::RequiresConfirmation && !confirmationGranted))
                return current.reason;
            if (current.canonicalSource != canonicalDecision.canonicalSource ||
                current.canonicalDestination != canonicalDecision.canonicalDestination ||
                current.canonicalExecutable != canonicalDecision.canonicalExecutable)
                return std::string("The canonical action resource changed before its effect.");
            if (request.type == ActionType::ExecuteProcess && current.processOutputLimitBytes < canonicalDecision.processOutputLimitBytes)
                return std::string("The process output allowance was reduced during execution.");
            if (IsBrowserAction(request.type))
            {
                const auto& previous = canonicalDecision.browser;
                const auto& now = current.browser;
                if ((!resource.empty() && !browser::IsApprovedUrl(resource, now)) || previous.approvedOrigins != now.approvedOrigins ||
                    previous.navigate != now.navigate || previous.interact != now.interact || previous.allowLoopback != now.allowLoopback ||
                    now.timeoutMs < previous.timeoutMs || now.maxTextBytes < previous.maxTextBytes ||
                    now.maxElements < previous.maxElements || now.maxValueBytes < previous.maxValueBytes)
                    return std::string("The interactive browser authority changed during this operation.");
            }
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
    if (auditLogger && contractAdmitted)
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
    else outcome.auditError = contractAdmitted ? "The action audit is unavailable." :
        "The rejected task envelope was not admitted to the evidence journal.";
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
    combined.processOutputLimitBytes = std::min(first.processOutputLimitBytes, second.processOutputLimitBytes);
    combined.browser = browser::IntersectSettings(first.browser, second.browser);
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
    const auto scopedDecision = scopedPolicy.Evaluate(request);
    if (request.type == ActionType::GenerateImage && scopedDecision.verdict != PolicyVerdict::Blocked &&
        globalDecision.canonicalDestination != scopedDecision.canonicalDestination)
    {
        auto refused = globalDecision;
        refused.verdict = PolicyVerdict::Blocked;
        refused.reason = "An image scope cannot replace the companion's fixed artifact destination.";
        return refused;
    }
    return MoreRestrictive(globalDecision, scopedDecision);
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
