#include "Computer/taskAcceptance.h"

#include "Computer/targetMatch.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace revia::computer
{
namespace
{

std::string Lower(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

TargetMatch OriginalDestination(const TaskContent& task, const ComputerObservation& observation)
{
    TargetDescriptor descriptor;
    descriptor.name = task.destination;
    const auto named = MatchTarget(observation.candidates, descriptor, TargetAffordance::Editable);
    if (named.outcome != TargetMatch::Outcome::NotFound)
        return named;
    descriptor.name.clear();
    descriptor.container = task.destination;
    descriptor.role = "edit";
    return MatchTarget(observation.candidates, descriptor, TargetAffordance::Editable);
}

} // namespace

const browser::BrowserElement* MatchOriginalBrowserField(
    const TaskContent& task, const browser::BrowserReceipt& receipt, std::string& refusal)
{
    refusal = "Browser placement requires exact supplied content, one named field, and one explicit original URL without submission.";
    if (task.requirement != ContentRequirement::ExactUserContent || task.value.empty() || task.destination.empty() ||
        task.submissionRequested || task.redacted.empty())
        return nullptr;
    // This acceptance criterion proves placement alone, not additional requested work.
    static const std::regex additionalWork(R"(\b(and|then|also|after|before)\b)", std::regex::icase);
    if (std::regex_search(task.redacted, additionalWork))
        return nullptr;
    static const std::regex urlPattern(R"(https?://[^\s'"<>]+)");
    auto urls = std::sregex_iterator(task.redacted.begin(), task.redacted.end(), urlPattern);
    if (urls == std::sregex_iterator())
        return nullptr;
    const auto originalOrigin = browser::UrlOrigin(urls->str());
    if (++urls != std::sregex_iterator() || originalOrigin.empty())
        return nullptr;
    refusal = "The current browser origin does not match the explicit origin in the original request.";
    if (browser::UrlOrigin(receipt.url) != originalOrigin || receipt.session.empty() || receipt.generation == 0 || receipt.uncertainEffect)
        return nullptr;
    const browser::BrowserElement* found = nullptr;
    for (const auto& element : receipt.elements)
    {
        if (!element.editable || Lower(element.name) != Lower(task.destination))
            continue;
        if (found != nullptr)
        {
            refusal = "The original browser destination matches multiple editable fields.";
            return nullptr;
        }
        found = &element;
    }
    refusal = found ? std::string{} : "The uniquely named original browser field is absent from the current observation.";
    return found;
}

goals::CompletionEvidence VerifyExactBrowserPlacement(
    const TaskContent& task, const goals::Goal& goal, const browser::BrowserReceipt& receipt)
{
    std::string refusal;
    const auto* field = MatchOriginalBrowserField(task, receipt, refusal);
    if (!field)
        return {false, std::move(refusal)};
    if (!field->valueAvailable || field->value != task.value)
        return {false, "Fresh browser readback did not establish the complete exact content in the original field."};
    for (auto entry = goal.steps.rbegin(); entry != goal.steps.rend(); ++entry)
    {
        const auto& step = *entry;
        if (step.action.type != actions::ActionType::BrowserFill || step.action.browser.value != task.value)
            continue;
        if (step.status != goals::StepStatus::Succeeded || step.attempts.empty())
            return {false, "The browser placement has no completed typed verification."};
        const auto& attempt = step.attempts.back();
        if (!attempt.executed || attempt.outcome != goals::VerificationOutcome::Verified ||
            attempt.checkedBy != goals::PostconditionKind::BrowserControlValueIs || step.action.browser.session != receipt.session ||
            step.action.browser.generation >= receipt.generation || step.action.browser.element != field->id ||
            browser::UrlOrigin(step.action.browser.url) != browser::UrlOrigin(receipt.url))
            return {false, "The current browser field is not tied to the verified placement in this owned session."};
        return {true, "Fresh browser readback confirms the exact supplied content in the unique original field and explicit origin; no "
                      "submission was requested."};
    }
    return {false, "No typed browser placement ties the exact content to the original request."};
}

goals::CompletionEvidence VerifyExactContentPlacement(
    const TaskContent& task, const goals::Goal& goal, const ComputerObservation& observation, const bool currentPlacementVerified)
{
    if (task.requirement != ContentRequirement::ExactUserContent || task.value.empty())
        return {false, "This request has no supported whole-task acceptance criterion. Verified intermediate steps do not establish the "
                       "requested outcome."};
    if (task.submissionRequested)
        return {false, "The request includes submission. Current draft text cannot establish that the intended destination received it."};
    if (task.destination.empty())
        return {false, "The original request did not identify a destination that can be independently verified."};
    if (!observation.Available())
        return {false, "A current admitted observation of the requested destination is unavailable."};
    const TargetMatch destination = OriginalDestination(task, observation);
    if (destination.outcome == TargetMatch::Outcome::Ambiguous)
        return {false, "The original destination matches multiple current editable controls; completion remains unresolved."};
    if (destination.outcome != TargetMatch::Outcome::Found || destination.candidate == nullptr)
        return {false, "The original destination was not found in the current observation."};
    if (!currentPlacementVerified)
        return {false, "A fresh readback did not confirm the exact user content in its original context."};

    for (auto entry = goal.steps.rbegin(); entry != goal.steps.rend(); ++entry)
    {
        const auto& step = *entry;
        if (step.action.type != actions::ActionType::SetControlText && step.action.type != actions::ActionType::TypeText)
            continue;
        if (step.action.value != task.value)
            continue;
        if (step.status != goals::StepStatus::Succeeded || step.attempts.empty())
            return {false, "The exact content placement has no completed typed verification."};
        const auto& attempt = step.attempts.back();
        if (!attempt.executed || attempt.outcome != goals::VerificationOutcome::Verified ||
            attempt.checkedBy != goals::PostconditionKind::ControlValueIs)
            return {false, "The exact content placement has no completed typed verification."};
        if (step.action.application.empty() || Lower(step.action.application) != Lower(observation.screen.foregroundApplication) ||
            step.action.control != destination.candidate->id)
            return {false, "The verified placement does not identify the original destination in the current application."};
        return {true, "Fresh draft readback confirmed the exact user content in the uniquely resolved original destination; the request "
                      "required no submission."};
    }
    return {false, "No typed placement record ties the exact user content to the requested destination."};
}

} // namespace revia::computer
