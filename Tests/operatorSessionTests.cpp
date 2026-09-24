#include "reviaSessionTestAccess.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <algorithm>
#include <cctype>

namespace
{

using namespace revia::actions;
using namespace revia::goals;
using revia::runtime::ReviaSession;
using revia::runtime::ReviaSessionTestAccess;
using revia::tests::Check;

void TestApprovalReachesExecution(const std::string& input, const bool approve,
    const bool cancel, const bool inScope = true, const bool write = false,
    const bool approveStep = true, const std::string& mode = "supervised",
    const bool remove = false)
{
    std::cout << "Operator session: " << input << " approve=" << approve
        << " cancel=" << cancel << " inScope=" << inScope << " write=" << write
        << " approveStep=" << approveStep << " mode=" << mode << std::endl;
    revia::tests::ScopedTestDirectory directory;
    const auto approved = directory.root / "approved";
    std::filesystem::create_directory(approved);
    const auto target = (inScope ? approved : directory.root) / "evidence.txt";
    {
        std::ofstream file(target);
        file << "operator evidence";
    }
    {
        std::ofstream file(directory.root / "capabilities.json");
        file << nlohmann::json{
            {"mode", mode},
            {"approvedRoots", {PathToUtf8(approved)}},
            {"autoApproveRiskThrough", "read_only"},
            {"desktopControl", {{"applicationLaunch", true}}},
            {"createMissingApprovedRoots", false}}.dump();
    }

    ReviaSession session;
    ReviaSessionTestAccess::PrepareOperator(session, directory.root,
        [&](const Goal&, const std::uint32_t iteration)
        {
            NextStep next;
            if (iteration == 0)
            {
                next.hasStep = true;
                next.step.description = "Read the fixture evidence";
                next.step.action.type = ActionType::ReadTextFile;
                next.step.action.source = target;
                next.step.check = next.step.action;
                next.step.expected = "operator evidence";
                if (write)
                {
                    next.step.action.type = ActionType::CreateDirectory;
                    next.step.action.source = approved / "created";
                    next.step.check.type = ActionType::ListDirectory;
                    next.step.check.source = approved;
                    next.step.expected = "created";
                }
                if (remove) next.step.action.type = ActionType::MoveToRecycleBin;
            }
            else next.finished = true;
            return next;
        });
    int confirmations = 0;
    bool waitingDuringDispatch = false;
    ReviaSessionTestAccess::ObserveActions(session, [&](const ActionRequest&, const bool beginning)
    {
        if (beginning && session.State() == revia::runtime::RuntimeState::WaitingForConfirmation)
            waitingDuringDispatch = true;
    });
    session.SetConfirmationHandler([&](const ActionRequest&, const PolicyDecision&)
    {
        ++confirmations;
        if (cancel) session.RequestStop();
        // Answered one at a time on purpose: this case counts prompts, and a standing
        // yes would suppress the very thing it is measuring.
        return (confirmations == 1 ? approve : approveStep)
            ? ConfirmationChoice::Allow : ConfirmationChoice::Decline;
    });
    if (mode == "owner_full_access") session.SetConfirmationHandler({});

    const auto result = ReviaSessionTestAccess::SubmitOperator(session, input);
    Check(confirmations == (mode == "owner_full_access" ? 0 : 1),
        "An operator task repeated approval or free mode asked for approval.");
    Check(!session.IsBusy(), "The completed operator command left the session busy.");
    Check(!waitingDuringDispatch, "An approved action still displayed Waiting during execution.");
    const auto goals = session.RecentGoals();
    if (!approve)
    {
        Check(!result.succeeded && goals.empty(),
            "Declining goal approval still started an operator run.");
    }
    else if (cancel)
    {
        Check(!result.succeeded && goals.size() == 1 &&
            goals.front().status == GoalStatus::Cancelled && goals.front().steps.empty(),
            "Approval erased Stop and started operator actions anyway.");
    }
    else if (!inScope || remove || (write && mode == "approved_scope"))
    {
        Check(!result.succeeded && goals.size() == 1 &&
            goals.front().stopReason == StopReason::PolicyBlocked,
            "Goal approval allowed an out-of-scope or unconfirmed action.");
        Check(!std::filesystem::exists(approved / "created"),
            "The refused operator step changed the filesystem.");
        Check(std::filesystem::exists(target), "The refused task deleted the fixture.");
    }
    else
    {
        Check(result.succeeded && goals.size() == 1 &&
            goals.front().status == GoalStatus::Succeeded &&
            goals.front().steps.size() == 1 &&
            goals.front().steps.front().status == StepStatus::Succeeded,
            "An approved operator request did not execute and verify its first step.");
        Check(std::filesystem::file_size(directory.root / "session-audit.jsonl") > 0,
            "The operator step was not audited.");
    }
}

// Opt-in only: uses the real session/policy/executors to open a local test page in Edge.
// The planner and approval answers are supplied by the fixture, not a live model/user.
void TestBrowserLaunch()
{
    revia::tests::ScopedTestDirectory directory;
    const auto page = directory.root / "browser-probe.html";
    const std::string title = "Revia browser probe " + NewActionId();
    {
        std::ofstream file(page);
        file << "<!doctype html><title>" << title << "</title><h1>" << title
            << "</h1><p>Revia opened this local test page through her action pipeline.</p>";
    }
    {
        std::ofstream file(directory.root / "capabilities.json");
        file << nlohmann::json{
            {"mode", "supervised"},
            {"approvedRoots", {PathToUtf8(directory.root)}},
            {"approvedApplications", {"msedge.exe"}},
            {"approvedControls", {{"msedge.exe", nlohmann::json::array()}}},
            {"autoApproveRiskThrough", "read_only"},
            {"desktopControl", {{"applicationLaunch", true}}},
            {"createMissingApprovedRoots", false}}.dump();
    }
    ReviaSession session;
    ReviaSessionTestAccess::PrepareOperator(session, directory.root,
        [&](const Goal&, const std::uint32_t iteration)
        {
            NextStep next;
            if (iteration == 0)
            {
                next.hasStep = true;
                next.step.description = "Open the local test page in Edge";
                next.step.action.type = ActionType::LaunchApplication;
                next.step.action.application = "msedge.exe";
                next.step.action.source = page;
                next.step.check.type = ActionType::InspectWindow;
                next.step.check.application = "msedge.exe";
                next.step.check.windowTitle = title;
                next.step.expected = title;
            }
            else next.finished = true;
            return next;
        });
    int approvals = 0;
    session.SetConfirmationHandler([&](const ActionRequest&, const PolicyDecision&)
    {
        ++approvals;
        return ConfirmationChoice::Allow;
    });
    const auto result = ReviaSessionTestAccess::SubmitOperator(session, "open Microsoft Edge");
    std::cout << "LIVE browser: " << result.text << '\n';
    for (const auto& goal : session.RecentGoals())
        for (const auto& step : goal.steps)
            for (const auto& attempt : step.attempts)
                std::cout << "  observed: " << attempt.observation
                    << " failure: " << attempt.failure << '\n';
    Check(result.succeeded && approvals == 2,
        "The real Edge launch/inspection did not complete through both approvals.");
}

void TestPlannerFailureIsExplained()
{
    revia::tests::ScopedTestDirectory directory;
    {
        std::ofstream file(directory.root / "capabilities.json");
        file << nlohmann::json{{"mode", "supervised"},
            {"approvedRoots", {PathToUtf8(directory.root)}},
            {"desktopControl", {{"applicationLaunch", true}}}}.dump();
    }
    ReviaSession session;
    ReviaSessionTestAccess::PrepareOperator(session, directory.root,
        [](const Goal&, std::uint32_t)
        {
            NextStep next;
            next.reason = "The foreground window is excluded from observation.";
            return next;
        });
    session.SetConfirmationHandler([](const auto&, const auto&)
        { return ConfirmationChoice::Allow; });
    const auto result = ReviaSessionTestAccess::SubmitOperator(session, "open Microsoft Edge");
    Check(!result.succeeded && result.text.find("foreground window is excluded") != std::string::npos,
        "An undecided goal hid the planner's explanation from the user.");
    Check(result.reasoning.find("No model call was involved") == std::string::npos,
        "The iterative operator falsely claimed that its planning path was deterministic.");
}

// Explicit opt-in: real model, Submit, policy, browser and verification. Only the
// approval answers are supplied here, limited to the requested address-bar workflow.
void TestLiveModelBrowserNavigation()
{
    const char* capabilities = std::getenv("REVIA_LIVE_OPERATOR_CAPABILITIES");
    const char* port = std::getenv("REVIA_LIVE_OPERATOR_PORT");
    Check(capabilities && port, "Live operator needs an explicit capabilities file and model port.");
    revia::tests::ScopedTestDirectory directory;
    std::filesystem::copy_file(capabilities, directory.root / "capabilities.json");
    ReviaSession session;
    ReviaSessionTestAccess::PrepareOperator(session, directory.root);
    ReviaSessionTestAccess::ConfigureLiveOperatorPlanner(session, std::stoi(port));
    session.SetDesktopApprovalHandler([](const revia::policy::ApprovalPrompt& prompt)
    {
        // The action approval below has already restricted text to the requested URL.
        return prompt.application == "msedge.exe" &&
            prompt.controlName == "Address and search bar";
    });
    session.SetConfirmationHandler([](const ActionRequest& action, const PolicyDecision&)
    {
        std::string keys = action.input.keys;
        std::transform(keys.begin(), keys.end(), keys.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        const bool address = action.value == "https://www.facebook.com" ||
            action.value == "https://www.facebook.com/" || action.value == "facebook.com" ||
            action.value == "www.facebook.com" || action.value == "https://facebook.com" ||
            action.value == "https://facebook.com/";
        const bool allowed = (action.type == ActionType::InspectWindow && action.application.empty()) ||
            (action.application == "msedge.exe" &&
                (action.type == ActionType::LaunchApplication || action.type == ActionType::FocusWindow ||
                 (action.type == ActionType::PressKeys &&
                    (keys == "ctrl+l" || keys == "ctrl+t" || keys == "enter")) ||
                 ((action.type == ActionType::TypeText || action.type == ActionType::SetControlText) &&
                    action.control == "view_1021" && address)));
        std::cout << "Live approval=" << allowed << " " << ToString(action.type)
                  << " app=" << action.application << " control=" << action.control << std::endl;
        // Per action, deliberately. This harness exists to check exactly which actions a
        // live run attempts, and a standing yes would stop it seeing them.
        return allowed ? ConfirmationChoice::Allow : ConfirmationChoice::Decline;
    });
    // Existing tabs are user-owned. A new tab makes this a navigation test even
    // when the requested site was already open elsewhere in the browser.
    ActionRequest blankTab;
    blankTab.type = ActionType::PressKeys;
    blankTab.application = "msedge.exe";
    blankTab.input.keys = "ctrl+t";
    const auto prepared = ReviaSessionTestAccess::Execute(session, blankTab);
    Check(prepared.succeeded,
        "Could not prepare a fresh browser tab: " + prepared.text);
    const auto result = ReviaSessionTestAccess::SubmitOperator(session,
        "open Microsoft Edge and pull up facebook");
    std::cout << result.text << '\n' << result.reasoning << std::endl;
    bool pageObserved = false;
    for (const auto& goal : session.RecentGoals())
        for (const auto& step : goal.steps)
            for (const auto& attempt : step.attempts)
            {
                std::cout << "STEP " << step.description
                          << "\nAction window: " << step.action.windowTitle
                          << "\nCheck window: " << step.check.windowTitle
                          << "\n" << attempt.observation
                          << "\nFailure: " << attempt.failure << std::endl;
                const auto start = attempt.observation.find("Window: ");
                if (attempt.verified && start != std::string::npos)
                {
                    std::string title = attempt.observation.substr(start,
                        attempt.observation.find('\n', start) - start);
                    std::transform(title.begin(), title.end(), title.begin(),
                        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                    pageObserved = pageObserved || title.find("facebook") != std::string::npos;
                }
            }
    Check(result.succeeded && pageObserved,
        "The real planner did not reach and observe the Facebook page in Edge.");
}

} // namespace

void RunOperatorSessionTests()
{
    TestPlannerFailureIsExplained();
    RunApplicationLocatorTests();
    TestApprovalReachesExecution("/operate read the fixture", false, false);
    TestApprovalReachesExecution("/operate read the fixture", true, false);
    TestApprovalReachesExecution("open Microsoft Edge", true, false);
    TestApprovalReachesExecution("/operate read the fixture", true, true);
    TestApprovalReachesExecution("open Microsoft Edge", true, true);
    TestApprovalReachesExecution("/operate read the fixture", true, false, false);
    TestApprovalReachesExecution("/operate create a directory", true, false, true, true);
    TestApprovalReachesExecution("/operate create a directory", true, false, true, true, false);
    TestApprovalReachesExecution("/operate create a directory", true, false, true, true,
        false, "owner_full_access");
    TestApprovalReachesExecution("message Joe on Messenger", true, false);
    TestApprovalReachesExecution("/operate delete the fixture", true, false, true, false,
        false, "supervised", true);
    TestApprovalReachesExecution("/operate delete the fixture", true, false, true, false,
        false, "owner_full_access", true);
    TestApprovalReachesExecution("/operate read outside scope", true, false, false, false,
        false, "owner_full_access");
    TestApprovalReachesExecution("/operate create a directory", true, false, true, true,
        true, "approved_scope");
    std::cout << "Operator session approval, cancellation and scope tests passed.\n";
    if (const char* live = std::getenv("REVIA_LIVE_BROWSER_SMOKE");
        live != nullptr && std::string(live) == "1")
    {
        TestBrowserLaunch();
    }
    if (const char* live = std::getenv("REVIA_LIVE_MODEL_OPERATOR");
        live && std::string(live) == "1") TestLiveModelBrowserNavigation();
}
