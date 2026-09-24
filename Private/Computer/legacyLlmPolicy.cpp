#include "Computer/legacyLlmPolicy.h"
#include "Core/utf8.h"

#include "Computer/taskContent.h"
#include "Planning/goalPlanner.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace revia::computer
{

namespace
{

constexpr std::size_t MaximumObservationCharacters = 400;
// What fits in a decision prompt beside the goal and the history. A browser
// advertises thousands of elements; the whole tree is not a usable answer to "what
// is on screen", and an unbounded one would push the history out of context.
constexpr std::size_t MaximumListedControls = 40;

std::string Bounded(std::string text, const std::size_t limit)
{
    for (char& character : text)
    {
        if (character == '\r' || character == '\n') character = ' ';
    }
    if (text.size() > limit)
    {
        revia::utf8::Truncate(text, limit);
        text += "...";
    }
    return text;
}

} // namespace

// Moved here whole from ReviaSession::BuildIterativeGoalContext. The only change is
// where the observation comes from: it is handed in rather than taken, so every
// provider in a shadow comparison reasons about the same screen and none of them can
// invalidate another's target by looking again.
std::string FormatLegacyContext(const ComputerTaskContext& context)
{
    nlohmann::json history = nlohmann::json::array();
    for (const ComputerAttempt& attempt : context.recentAttempts)
    {
        history.push_back({
            {"step", attempt.description},
            {"action", actions::ToString(attempt.action)},
            {"expected", Bounded(attempt.expected, MaximumObservationCharacters)},
            {"status", goals::ToString(attempt.status)},
            {"executed", attempt.executed},
            {"verified", attempt.verified},
            // What the check actually saw, which is the only part of this the next
            // decision may treat as fact.
            {"observed", Bounded(attempt.observed, MaximumObservationCharacters)},
            {"failure", Bounded(attempt.failure, MaximumObservationCharacters)}});
    }

    nlohmann::json roots = nlohmann::json::array();
    for (const auto& root : context.scope.approvedRoots)
        roots.push_back(actions::PathToUtf8(root));

    const actions::windows::DesktopObservation& screen = context.observation.screen;
    nlohmann::json observation;
    if (!screen.succeeded)
    {
        observation = {
            {"available", false},
            {"reason", Bounded(screen.failure.empty()
                ? std::string("The desktop could not be observed.")
                : screen.failure, MaximumObservationCharacters)}};
    }
    else if (context.observation.withheld)
    {
        // The same exclusion list ordinary perception honours, for the same reason. A
        // goal run is not a reason to read the password manager the user just switched
        // to, and the window is reported as withheld rather than described, so the
        // decision knows it is blind here instead of concluding the screen is empty.
        observation = {
            {"available", false},
            {"reason", "The window in front is excluded from observation, so nothing "
                       "about its contents is available."}};
    }
    else
    {
        observation = {
            {"available", true},
            {"application", Bounded(screen.foregroundApplication, 120)},
            {"title", Bounded(screen.foregroundTitle, MaximumObservationCharacters)},
            {"window", {
                {"left", screen.windowLeft},
                {"top", screen.windowTop},
                {"right", screen.windowRight},
                {"bottom", screen.windowBottom}}},
            {"controls", screen.Describe(MaximumListedControls)}};
    }

    // Constrain proposed control operations to observed Windows patterns. This only
    // removes unusable choices; execution still rechecks policy and target identity.
    nlohmann::json controlTargets = {
        {"invoke_control", nlohmann::json::array()},
        {"set_control_text", nlohmann::json::array()},
        {"type_text", nlohmann::json::array()}};
    if (observation.value("available", false))
    {
        for (const ObservedCandidate& candidate : context.observation.candidates)
        {
            if (candidate.mayInvoke)
                controlTargets["invoke_control"].push_back(candidate.id);
            if (candidate.maySetText)
                controlTargets["set_control_text"].push_back(candidate.id);
            if (candidate.mayType)
                controlTargets["type_text"].push_back(candidate.id);
        }
    }
    observation["control_targets"] = std::move(controlTargets);

    // Acting is not achieving. An action that ran, succeeded, and left the screen
    // identical has made no progress, and saying so is what stops the loop from
    // repeating it until a budget runs out.
    if (context.observation.changedSinceLastDecision.has_value())
    {
        observation["screen_changed_since_last_decision"] =
            *context.observation.changedSinceLastDecision;
    }

    // What the runtime is holding for this task, described and never revealed.
    //
    // This is the half of the exact-content repair that reaches the model. Told that a
    // message of this length exists and that the only thing it may write where the
    // words go is a fixed token, a planner chooses a destination -- which is the
    // question it is competent to answer. Told nothing, as it was, it writes a
    // plausible sentence of its own, types it, and reports the task done.
    nlohmann::json prepared;
    if (context.preparedContent.held)
    {
        prepared = {
            {"held", true},
            {"kind", context.preparedContent.kind},
            {"length", context.preparedContent.length},
            {"enter_with", PreparedContentToken}};
        if (!context.preparedContent.destination.empty())
        {
            prepared["destination"] = context.preparedContent.destination;
        }
    }
    else
    {
        prepared = {{"held", false}};
    }

    return nlohmann::json({
        {"goal", context.subgoal},
        {"iteration", context.iteration},
        {"observation", std::move(observation)},
        {"prepared_content", std::move(prepared)},
        {"steps_taken", context.stepsTaken},
        {"actions_left", context.actionsLeft},
        {"retries_left", context.retriesLeft},
        {"scope", {
            {"roots", roots},
            {"applications", context.scope.approvedApplications},
            {"controls", context.scope.approvedControls},
            {"mode", actions::ToString(context.scope.mode)},
            {"auto_approve_risk_through",
                actions::ToString(context.scope.autoApproveRiskThrough)},
            {"desktop_control", {
                {"application_launch", context.scope.desktopControl.applicationLaunch},
                {"keyboard", context.scope.desktopControl.keyboard},
                {"pointer", context.scope.desktopControl.pointer}}}}},
        {"history", std::move(history)}}).dump();
}

LegacyLlmComputerPolicy::LegacyLlmComputerPolicy(PlannerCall planner)
    : plannerCall(std::move(planner))
{
}

ComputerDecision LegacyLlmComputerPolicy::Decide(
    const ComputerTaskContext& context, std::stop_token stopToken)
{
    ComputerDecision decision;
    decision.provider = Name();
    if (!plannerCall)
    {
        decision.code = ComputerReasonCode::ProviderUnavailable;
        decision.detail = "The next step could not be decided.";
        return decision;
    }

    const responseOutput answer = plannerCall(FormatLegacyContext(context), std::move(stopToken));
    // Carried before the answer is inspected, so a failed or unusable decision still
    // reports what it cost. Planning that produced nothing was still paid for.
    decision.tokens = answer.TotalTokens();
    decision.costReported = answer.bTokensReported;
    if (!answer.bSuccess)
    {
        decision.code = ComputerReasonCode::ProviderFailed;
        decision.detail = answer.reason.empty()
            ? "The next step could not be decided." : answer.reason;
        return decision;
    }

    const planning::ParsedNextStep parsed = planning::GoalPlanner::ParseNextStep(answer.response);
    if (!parsed.succeeded)
    {
        decision.code = ComputerReasonCode::MalformedProviderOutput;
        decision.detail = parsed.error;
        return decision;
    }
    if (parsed.finished)
    {
        decision.kind = ComputerDecisionKind::ProposeCompletion;
        decision.detail = parsed.error;
        return decision;
    }
    if (parsed.step.action.type == actions::ActionType::Unknown)
    {
        // A usable answer that proposes nothing. Distinct from a parse failure, and
        // distinct from finishing: she looked and had no next move.
        decision.code = ComputerReasonCode::NoActionProposed;
        decision.detail = parsed.error.empty()
            ? "No next action was proposed." : parsed.error;
        return decision;
    }

    decision.kind = ComputerDecisionKind::ProposeAction;
    decision.step = parsed.step;
    return decision;
}

} // namespace revia::computer
