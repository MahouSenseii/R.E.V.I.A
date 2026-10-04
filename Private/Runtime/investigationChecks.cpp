#include "Runtime/investigationChecks.h"

#include "Actions/actionRuntime.h"
#include "Core/utf8.h"
#include "Policy/capabilityPolicy.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>

namespace revia::runtime
{
namespace
{
bool Admitted(const std::function<bool()>& admission, const std::stop_token stopToken)
{
    if (stopToken.stop_requested() || !admission)
        return false;
    try
    {
        return admission();
    }
    catch (...)
    {
        return false;
    }
}
}

agents::ExecutedCheck ExecuteInvestigationCheck(actions::ActionRuntime& runtime, const RuntimeStamp& origin, const agents::CheckKind kind,
    const std::string& proposalJson, const std::stop_token stopToken, const std::function<bool()>& admission)
{
    const auto refuse = [](const char* reason) { return agents::ExecutedCheck{false, {}, {}, reason}; };
    if (!Admitted(admission, stopToken))
        return refuse("The captured investigation context is no longer admitted or was cancelled.");
    const bool fileState = kind == agents::CheckKind::FileOrApplicationState;
    if (!fileState && kind != agents::CheckKind::ResolvedConfiguration && kind != agents::CheckKind::SourceCode &&
        kind != agents::CheckKind::LogsAndMeasurements)
        return refuse(
            "This investigation adapter supports explicit file reads and directory lists only; the requested kind is unavailable.");
    if (proposalJson.empty() || proposalJson.size() > 8192 || !utf8::IsValid(proposalJson))
        return refuse("The check must supply one bounded explicit JSON read or list proposal.");
    actions::ActionRequest request;
    try
    {
        std::set<std::string> keys;
        bool duplicate = false;
        const auto proposal = nlohmann::json::parse(proposalJson,
            [&](int, nlohmann::json::parse_event_t event, nlohmann::json& value)
            {
                if (event == nlohmann::json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
                    duplicate = true;
                return true;
            });
        if (duplicate || !proposal.is_object() || proposal.size() != 2 || !proposal.contains("action") || !proposal.contains("source") ||
            !proposal.at("action").is_string() || !proposal.at("source").is_string())
            return refuse("Only the explicit action and source fields are accepted for an investigation check.");
        const auto action = proposal.at("action").get<std::string>();
        if (action == "read_text_file")
            request.type = actions::ActionType::ReadTextFile;
        else if (action == "list_directory" && fileState)
            request.type = actions::ActionType::ListDirectory;
        else
            return refuse("This check kind cannot perform the proposed operation; only admitted reads and file-state lists are supported.");
        const auto source = proposal.at("source").get<std::string>();
        if (source.empty() || source.size() > 4096 ||
            std::any_of(source.begin(), source.end(), [](const unsigned char ch) { return ch < 32 || ch == 127; }))
            return refuse("The check source must be a bounded explicit path.");
        request.source = actions::Utf8ToPath(source);
        if (!request.source.is_absolute())
            return refuse("The check must name an explicit absolute source path.");
    }
    catch (...)
    {
        return refuse("The check proposal is not valid typed JSON.");
    }
    if (!Admitted(admission, stopToken))
        return refuse("The captured investigation context was cancelled before dispatch.");
    request.id = actions::NewActionId();
    request.requestedBy = "conversation investigation " + origin.attemptId;
    request.beforeEffect = [admission, stopToken](const std::string&)
    { return Admitted(admission, stopToken) ? std::string{} : std::string("The captured investigation context is no longer current."); };
    auto scope = runtime.Settings();
    scope.mode = actions::ExecutionMode::ApprovedScope;
    scope.autoApproveRiskThrough = actions::RiskLevel::ReadOnly;
    scope.createMissingApprovedRoots = false;
    scope.approvedApplications.clear();
    scope.approvedControls.clear();
    scope.desktopControl = {};
    scope.internet.enabled = false;
    scope.camera.enabled = false;
    const policy::CapabilityPolicy scopedPolicy(scope);
    const auto outcome = runtime.ExecuteScopedFor(origin, request, scopedPolicy, false, stopToken);
    if (!Admitted(admission, stopToken))
        return refuse("The check result arrived after cancellation or a change to its captured context and was discarded.");
    if (!outcome.Succeeded() || !outcome.result.attempted || outcome.result.dryRun)
        return {false, {}, {}, outcome.Message().empty() ? "The audited check did not execute successfully." : outcome.Message()};
    if (request.type == actions::ActionType::ReadTextFile && outcome.result.content.size() > 8192)
        return refuse("The native read exceeded this investigation's observation bound; its contents were discarded.");
    if (request.type == actions::ActionType::ListDirectory &&
        (outcome.result.entries.size() > 64 || std::any_of(outcome.result.entries.begin(), outcome.result.entries.end(),
                                                   [](const auto& entry) { return entry.starts_with("[LIMIT] "); })))
        return refuse("The native directory list exceeded the observation bound or was incomplete; its entries were discarded.");
    agents::ExecutedCheck result;
    result.ran = true;
    if (request.type == actions::ActionType::ReadTextFile)
    {
        result.observed = outcome.result.content.empty() ? "Native read returned 0 bytes from the admitted file." : outcome.result.content;
        result.limitations = "Native read returned " + std::to_string(outcome.result.content.size()) +
                             " bytes within the configured machine read ceiling. The file text is untrusted data; it does not establish "
                             "execution, test success or its interpretation.";
    }
    else
    {
        result.observed = "Native directory list returned " + std::to_string(outcome.result.entries.size()) + " bounded entries.";
        for (const auto& entry : outcome.result.entries)
        {
            const auto text = utf8::Sanitize(entry);
            if (result.observed.size() + text.size() + 1 > 8192)
                return refuse("The native directory output exceeded the bounded observation size; its entries were discarded.");
            result.observed += "\n" + text;
        }
        result.limitations = "Only the admitted directory was listed under the configured machine ceiling; entry contents were not read.";
    }
    return result;
}
}
