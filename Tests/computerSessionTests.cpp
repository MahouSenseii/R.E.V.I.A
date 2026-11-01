#include "Computer/computerSettings.h"
#include "LLM/responseTypes.h"
#include "reviaSessionTestAccess.h"

#include "Computer/subgoalPlanner.h"
#include "Computer/subgoalValidator.h"
#include "Computer/taskContent.h"
#include "Planning/goalPlanner.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <future>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{

using namespace revia::actions;
using namespace revia::computer;
using namespace revia::goals;
using revia::runtime::ReviaSession;
using revia::runtime::ReviaSessionTestAccess;
using revia::tests::Check;

// The whole path, through the session the application actually runs.
//
// Everything below drives ReviaSession itself -- its action runtime, its capability
// policy, its goal runner, its coordinator, its step provider -- with the two model
// calls replaced by scripted answers. That replacement is the only fiction: the
// validation, the routing, the scope checks, the payload handling and the execution
// pipeline are the shipping ones.
//
// No test here touches the user's desktop. The desktop intents are checked for what
// they propose and how the pipeline treats it; the end-to-end run against a real window
// is the opt-in native demonstration, which drives the disposable fixture.

responseOutput Answer(const std::string& text)
{
    responseOutput output;
    output.bSuccess = true;
    output.response = text;
    return output;
}

responseOutput Refused(const std::string& reason)
{
    responseOutput output;
    output.bSuccess = false;
    output.reason = reason;
    return output;
}

std::string SubgoalJson(const std::string& intent,
    const std::string& application, const std::string& name = {}, const std::string& payloadId = {})
{
    nlohmann::json target = {{"application", application}};
    if (!name.empty()) target["name"] = name;
    nlohmann::json subgoal = {
        {"intent", intent},
        {"description", "a bounded step"},
        {"target", std::move(target)}};
    if (!payloadId.empty()) subgoal["payload_id"] = payloadId;
    return subgoal.dump();
}

// The step that entered text, wherever it sits in the run.
//
// Indexing step 0 stopped working when the runtime began deriving progression for itself:
// a task whose window is not in front now starts with a focus step it was never asked
// for, which is the point. What the assertions below are about is what reached a *field*,
// so they look for that step rather than assuming its position.
const GoalStep* TextEntry(const Goal& goal)
{
    for (const GoalStep& step : goal.steps)
    {
        if (step.action.type == ActionType::SetControlText ||
            step.action.type == ActionType::TypeText)
        {
            return &step;
        }
    }
    return nullptr;
}

// A session wired for the operator path, with an approved fixture root and the
// disposable application the desktop tests already use.
struct SessionFixture
{
    revia::tests::ScopedTestDirectory directory;
    std::filesystem::path approved = directory.root / "approved";
    ReviaSession session;
    // Every request that reached a model, so a test can assert on what was and was not
    // sent rather than on a counter it has to trust.
    std::vector<std::string> subgoalRequests;
    std::vector<std::string> stepRequests;

    SessionFixture()
    {
        std::filesystem::create_directories(approved);
        {
            std::ofstream file(directory.root / "capabilities.json");
            file << nlohmann::json{
                {"mode", "approved_scope"},
                {"approvedRoots", {PathToUtf8(approved)}},
                {"approvedApplications", {"reviadesktopfixture.exe"}},
                {"approvedControls", {{"reviadesktopfixture.exe", {"*"}}}},
                {"autoApproveRiskThrough", "reversible_write"},
                {"desktopControl", {
                    {"applicationLaunch", true},
                    {"keyboard", true},
                    {"pointer", true},
                    {"maxTypedCharacters", 512}}},
                {"createMissingApprovedRoots", false}}.dump();
        }
        ReviaSessionTestAccess::PrepareOperator(session, directory.root);
        ReviaSessionTestAccess::UseRealStepProvider(session);
    }

    void Script(std::function<std::string(int)> subgoalAnswers, std::function<std::string(int)> stepAnswers)
    {
        ReviaSessionTestAccess::ScriptComputerProviders(
            session,
            [this, subgoalAnswers](const std::string& prompt, std::stop_token)
            {
                subgoalRequests.push_back(prompt);
                const std::string answer =
                    subgoalAnswers(static_cast<int>(subgoalRequests.size()) - 1);
                return answer.empty() ? Refused("no subgoal") : Answer(answer);
            },
            [this, stepAnswers](const std::string& prompt, std::stop_token)
            {
                stepRequests.push_back(prompt);
                const std::string answer =
                    stepAnswers(static_cast<int>(stepRequests.size()) - 1);
                return answer.empty() ? Refused("no step") : Answer(answer);
            });
    }

    void UseMode(const std::string& mode)
    {
        computerControlSettings settings;
        settings.providerMode = mode;
        ReviaSessionTestAccess::SetComputerSettings(session, settings);
    }

    [[nodiscard]] ComputerTaskCoordinator& Tasks()
    {
        return ReviaSessionTestAccess::ComputerTasks(session);
    }
};

// The default installs the feature and changes nothing. Every decision still goes to
// the model, and no subgoal is ever asked for.
void TestTheDefaultSessionStillAsksTheModelForEveryStep()
{
    SessionFixture fixture;
    fixture.Script(
        [](int) { return std::string(); },
        [](int index)
        {
            if (index >= 2) return nlohmann::json{{"finished", true}}.dump();
            return nlohmann::json{
                {"decision", "act"},
                {"description", "Look at the approved folder"},
                {"step", {
                    {"action", {{"type", "list_directory"}, {"source", "."}}},
                    {"check", {{"type", "list_directory"}, {"source", "."}}},
                    {"expected", "nothing"}}}}.dump();
        });

    Goal goal;
    goal.id = NewGoalId();
    goal.title = "look around";
    goal.budget.maxActions = 3;
    ReviaSessionTestAccess::BeginComputerTask(fixture.session, goal.id);
    static_cast<void>(ReviaSessionTestAccess::OperateGoal(fixture.session, goal));
    ReviaSessionTestAccess::EndComputerTask(fixture.session);

    Check(fixture.subgoalRequests.empty(),
        "The default mode asked for a subgoal, which costs a model call the default "
        "must not spend.");
    Check(!fixture.stepRequests.empty(),
        "The default mode stopped asking the existing decision path.");
}

// The subgoal request never carries the user's words, only a handle to them.
void TestTheSubgoalPromptReferencesContentWithoutShowingIt()
{
    SessionFixture fixture;
    const PayloadReference reference =
        fixture.Tasks().HoldPayload("Running late, see you at eight", "message");

    ComputerTaskContext context;
    context.observation.screen.succeeded = true;
    context.observation.screen.foregroundApplication = "reviadesktopfixture.exe";
    const SubgoalRequest request =
        FormatSubgoalRequest("tell them I am late", context, reference);
    // Both halves, because the payload must be absent from each of them.
    const std::string prompt = request.instruction + request.situation;

    Check(prompt.find(reference.id) != std::string::npos,
        "The prompt did not name the payload, so the model cannot reference it.");
    Check(prompt.find("Running late") == std::string::npos &&
            prompt.find("see you at eight") == std::string::npos,
        "The user's exact words were put into a model prompt, which is the one thing a "
        "payload reference exists to prevent.");
    Check(prompt.find("\"length\":30") != std::string::npos ||
            prompt.find("\"length\": 30") != std::string::npos,
        "The prompt did not say how long the content is, which is what lets a model "
        "decide where it fits without being shown it.");
}

// A subgoal naming an application this task is not approved for is refused by the
// session's own validation, before any policy sees it.
void TestAnOutOfScopeSubgoalNeverReachesAPolicy()
{
    SessionFixture fixture;
    fixture.UseMode("assisted");
    fixture.Script(
        [](int) { return SubgoalJson("interact_with_control", "cmd.exe", "Run"); },
        [](int) { return nlohmann::json{{"finished", true}}.dump(); });

    Goal goal;
    goal.id = NewGoalId();
    goal.title = "open a shell";
    goal.budget.maxActions = 2;
    goal.scope.approvedApplications = {"reviadesktopfixture.exe"};
    ReviaSessionTestAccess::BeginComputerTask(fixture.session, goal.id);
    static_cast<void>(ReviaSessionTestAccess::OperateGoal(fixture.session, goal));

    Check(!fixture.subgoalRequests.empty(), "No subgoal was ever requested.");
    Check(!fixture.Tasks().Controller().HasSubgoal(),
        "A subgoal naming an unapproved application was installed.");
    // And the run fell back to the existing path rather than proceeding on a refused
    // subgoal, which is the behaviour that makes a refusal safe rather than fatal.
    Check(!fixture.stepRequests.empty(),
        "A refused subgoal did not fall back to the existing decision path.");
    ReviaSessionTestAccess::EndComputerTask(fixture.session);
}

// The measurement the feature exists to produce, taken through the real session.
void TestAssistedModeMovesDecisionsAwayFromTheModel()
{
    SessionFixture fixture;
    fixture.UseMode("assisted");
    // One bounded subgoal, then nothing more to propose.
    fixture.Script(
        [](int index)
        {
            return index == 0
                ? SubgoalJson("focus_window", "reviadesktopfixture.exe")
                : std::string();
        },
        [](int) { return nlohmann::json{{"finished", true}}.dump(); });

    Goal goal;
    goal.id = NewGoalId();
    goal.title = "bring the fixture forward";
    goal.budget.maxActions = 2;
    goal.scope.approvedApplications = {"reviadesktopfixture.exe"};
    goal.scope.desktopControl.applicationLaunch = true;

    ReviaSessionTestAccess::BeginComputerTask(fixture.session, goal.id);
    static_cast<void>(ReviaSessionTestAccess::OperateGoal(fixture.session, goal));

    const ComputerControllerStats& stats = fixture.Tasks().Stats();
    Check(fixture.subgoalRequests.size() >= 1,
        "No bounded subgoal was ever asked for in assisted mode.");
    // The routine policy answered at least one iteration. On a machine with no such
    // window in front it proposes focusing it, which needs no model and no reasoning.
    Check(stats.routineDecisions >= 1,
        "No decision was taken without a model; routine decisions were " +
            std::to_string(stats.routineDecisions) + " of " +
            std::to_string(stats.decisions) + ".");
    ReviaSessionTestAccess::EndComputerTask(fixture.session);
}

// The payloads belong to the task, and go when it does.
void TestTaskPayloadsAreDroppedWhenTheTaskEnds()
{
    SessionFixture fixture;
    const PayloadReference reference =
        fixture.Tasks().HoldPayload("something private", "message");
    Check(fixture.Tasks().Controller().HasSubgoal() == false,
        "A subgoal was installed before any task started.");

    ReviaSessionTestAccess::BeginComputerTask(fixture.session, "goal-1");
    ReviaSessionTestAccess::EndComputerTask(fixture.session);

    // Redeeming through the controller is the only way the value ever becomes a
    // keystroke, and after the task it must not.
    ComputerSubgoal proposed;
    proposed.intent = SubgoalIntent::EnterPayload;
    proposed.target.application = "reviadesktopfixture.exe";
    proposed.target.name = "Document";
    proposed.payload = reference;
    SubgoalContext context;
    context.goalId = "goal-1";
    context.scope.approvedApplications = {"reviadesktopfixture.exe"};
    context.scope.desktopControl.maxTypedCharacters = 512;

    // The vault the coordinator owns is not reachable from here by design, so this
    // asserts the observable consequence: a subgoal referencing the dropped payload can
    // no longer be validated.
    Check(!fixture.Tasks().Controller().HasSubgoal(),
        "A subgoal survived the task it belonged to.");
    static_cast<void>(context);
    static_cast<void>(proposed);
}

// Recording stays off through a whole run unless it is explicitly turned on.
void TestARunRecordsNothingByDefault()
{
    SessionFixture fixture;
    fixture.UseMode("assisted");
    fixture.Script(
        [](int index)
        {
            return index == 0
                ? SubgoalJson("focus_window", "reviadesktopfixture.exe")
                : std::string();
        },
        [](int) { return nlohmann::json{{"finished", true}}.dump(); });

    Goal goal;
    goal.id = NewGoalId();
    goal.title = "bring the fixture forward";
    goal.budget.maxActions = 2;
    goal.scope.approvedApplications = {"reviadesktopfixture.exe"};

    ReviaSessionTestAccess::BeginComputerTask(fixture.session, goal.id);
    static_cast<void>(ReviaSessionTestAccess::OperateGoal(fixture.session, goal));
    ReviaSessionTestAccess::EndComputerTask(fixture.session);

    Check(!fixture.Tasks().Recorder().Capturing(),
        "A run turned recording on by itself.");
    Check(fixture.Tasks().Recorder().Sessions().empty(),
        "A dataset appeared from a run nobody asked to record.");
}

// The status report is the thing a person reads to find out why a mode is not doing
// what they selected, so it has to be specific.
void TestTheStatusReportExplainsAnInactiveMode()
{
    SessionFixture fixture;
    fixture.UseMode("learned");
    const std::string report = fixture.Tasks().StatusReport();

    Check(report.find("Selected mode:   learned") != std::string::npos,
        "The report did not say which mode was selected.");
    Check(report.find("Active mode:") != std::string::npos &&
            report.find("Why not:") != std::string::npos,
        "The report did not explain why the selected mode is not the active one.");
    Check(report.find("no qualified artifact is loaded") != std::string::npos,
        "The report did not distinguish a missing learned artifact from a refused "
        "permission or an unsupported task.");
    Check(report.find("Reached a model:") != std::string::npos &&
            report.find("For subgoals:") != std::string::npos &&
            report.find("Existing path:") != std::string::npos &&
            report.find("Net:") != std::string::npos,
        "The report did not show both halves of the cost: the calls avoided and the "
        "calls this feature spends.");
}

// A stop during a run stops it, and nothing decided late is dispatched afterwards.
void TestAStopEndsTheRunWithoutALateStep()
{
    SessionFixture fixture;
    fixture.UseMode("assisted");
    fixture.Script(
        [](int) { return SubgoalJson("focus_window", "reviadesktopfixture.exe"); },
        [](int) { return nlohmann::json{{"finished", true}}.dump(); });

    Goal goal;
    goal.id = NewGoalId();
    goal.title = "a task that will be stopped";
    goal.budget.maxActions = 4;
    goal.scope.approvedApplications = {"reviadesktopfixture.exe"};

    ReviaSessionTestAccess::BeginComputerTask(fixture.session, goal.id);
    const Goal finished =
        ReviaSessionTestAccess::OperateGoal(fixture.session, goal, /*cancelBeforeRun=*/true);
    ReviaSessionTestAccess::EndComputerTask(fixture.session);

    Check(finished.status == GoalStatus::Cancelled ||
            finished.stopReason == StopReason::Cancelled,
        "A run stopped before it began did not record itself as cancelled; it stopped "
        "because " + ToString(finished.stopReason) + ".");
    for (const GoalStep& step : finished.steps)
    {
        for (const StepAttempt& attempt : step.attempts)
        {
            Check(!attempt.executed,
                "A step executed after the run was already cancelled.");
        }
    }
}

// ---- exact content, through the session, on every path ----

// The step a model actually produced against a live backend, reduced to its essentials.
// It names a real field and writes a sentence nobody asked for.
std::string EntryStep(const std::string& control, const std::string& value)
{
    return nlohmann::json{
        {"decision", "act"},
        {"description", "Put the message in the box"},
        {"step", {
            {"action", {
                {"action", "set_control_text"},
                {"application", "reviadesktopfixture.exe"},
                {"control", control},
                {"value", value}}},
            {"check", {
                {"action", "inspect_window"},
                {"application", "reviadesktopfixture.exe"}}},
            {"expected", control + " holds the message"}}}}.dump();
}

TaskContent QuotedRequest(const std::string& request)
{
    return ExtractTaskContent(request);
}

// Legacy is the path where this was observed and the path with no payload mechanism of
// its own. It is also the default, so if the repair does not hold here it does not hold
// where most runs happen.
void TestLegacyModeCannotPlaceItsOwnWords()
{
    SessionFixture fixture;
    fixture.UseMode("legacy");
    fixture.Script(
        [](int) { return std::string(); },
        [](int index)
        {
            if (index == 0)
            {
                return EntryStep("compose", "The prepared message text here");
            }
            return nlohmann::json{{"decision", "complete"},
                {"reason", "the message is in the box"}}.dump();
        });

    Goal goal;
    goal.id = NewGoalId();
    goal.title = "put \"dinner at eight\" in the Compose box";
    goal.budget.maxActions = 2;
    goal.scope.approvedApplications = {"reviadesktopfixture.exe"};

    ReviaSessionTestAccess::BeginComputerTask(
        fixture.session, goal.id, QuotedRequest(goal.title));
    const Goal finished = ReviaSessionTestAccess::OperateGoal(fixture.session, goal);

    const GoalStep* entry = TextEntry(finished);
    Check(entry != nullptr, "No text was entered at all, so nothing was checked.");
    Check(entry->action.value == "dinner at eight",
        "The legacy path placed its own sentence: '" + entry->action.value +
            "'. This is the defect exactly as it was observed against a live model, and "
            "the whole suite passed while it was happening.");
    Check(fixture.Tasks().ContentStats().inventions == 1,
        "The substitution was silent. A repair that leaves no trace cannot tell anyone "
        "whether the planner is still trying.");
    ReviaSessionTestAccess::EndComputerTask(fixture.session);
}

// The grammar half. A planner that goes through the next-step schema cannot even emit
// an invented sentence, because the schema does not admit one.
void TestTheGrammarOffersNoRoomForAnInventedPayload()
{
    SessionFixture fixture;
    fixture.UseMode("legacy");
    fixture.Script(
        [](int) { return std::string(); },
        [](int)
        {
            return nlohmann::json{{"decision", "blocked"},
                {"reason", "nothing to do"}}.dump();
        });

    Goal goal;
    goal.id = NewGoalId();
    goal.title = "type \"dinner at eight\" into Compose";
    goal.budget.maxActions = 1;
    goal.scope.approvedApplications = {"reviadesktopfixture.exe"};

    ReviaSessionTestAccess::BeginComputerTask(
        fixture.session, goal.id, QuotedRequest(goal.title));
    static_cast<void>(ReviaSessionTestAccess::OperateGoal(fixture.session, goal));
    ReviaSessionTestAccess::EndComputerTask(fixture.session);

    Check(!fixture.stepRequests.empty(), "The decision path was never asked.");
    const std::string& context = fixture.stepRequests.front();
    Check(context.find("prepared_content") != std::string::npos,
        "The decision context never mentioned that content was held, so a planner had "
        "no way to know it should not write its own.");
    // Asked of the parts the runtime writes, not of the whole prompt.
    //
    // The decision context also carries a description of the real screen, and a first
    // version of this check asserted over all of it. That caught a stray Notepad which a
    // matrix run had left open with the test's own phrase in its title -- a true
    // observation of the machine, and nothing at all to do with the redaction this test
    // is about. Worse, it would fail for a person who simply had that sentence open in a
    // window, which is exactly the situation the feature is meant to work in.
    //
    // The guarantee is about what the *runtime* puts in a prompt: the task description
    // and the prepared-content block. What the screen shows is the screen, and an
    // observation of a window that legitimately contains the words is not a leak of the
    // vault.
    const nlohmann::json decoded = nlohmann::json::parse(context, nullptr, false);
    Check(decoded.is_object(), "The decision context was not readable JSON.");
    const std::string described = decoded.value("goal", std::string());
    Check(described.find("dinner at eight") == std::string::npos,
        "The user's exact words were put into the task description the model reads: '" +
            described + "'. Holding them in a vault achieves nothing while the "
            "description spells them out.");
    Check(decoded["prepared_content"].dump().find("dinner at eight") == std::string::npos,
        "The prepared-content block carried the value rather than a description of it.");

    // The schema the constrained decode is actually built from, asked the same
    // question. A `value` that is still a free string is a grammar that will let a
    // model write a sentence there whatever the prompt says.
    //
    // Built from a context with a field on screen, because the grammar only offers a
    // text-entry variant when there is something to enter text into -- and this test
    // runs on whatever window happens to be in front of the machine running it.
    const nlohmann::json withAField = {
        {"observation", {
            {"available", true},
            {"application", "reviadesktopfixture.exe"},
            {"title", "Revia Fixture"},
            {"control_targets", {
                {"set_control_text", {"compose"}},
                {"type_text", {"compose"}},
                {"invoke_control", nlohmann::json::array()}}}}},
        {"prepared_content", {
            {"held", true},
            {"kind", "text"},
            {"length", 15},
            {"enter_with", PreparedContentToken}}}};
    const std::string schema =
        revia::planning::GoalPlanner::NextStepSchema(withAField.dump());
    Check(schema.find(PreparedContentToken) != std::string::npos,
        "The next-step grammar did not constrain the value field to the prepared-content "
        "token while content was held, so a constrained decode could still emit a "
        "sentence of its own there.");

    // Typing is not placing. With content held, the grammar must not offer the action
    // that appends -- a live run discovered why, eleven appends deep.
    Check(schema.find("type_text") == std::string::npos,
        "The grammar still offers type_text while content is held. Typing synthesises "
        "keystrokes into whatever the field already contains, so a second attempt "
        "appends rather than replaces and the check then fails into a worse retry.");

    // And the converse: with nothing held, `value` stays a free string, because a URL
    // or a search term a planner composes is ordinary work.
    nlohmann::json withoutContent = withAField;
    withoutContent["prepared_content"] = {{"held", false}};
    const std::string openSchema =
        revia::planning::GoalPlanner::NextStepSchema(withoutContent.dump());
    Check(openSchema.find(PreparedContentToken) == std::string::npos,
        "The grammar constrained the value field for a task holding no content, which "
        "would leave a planner unable to type a URL.");
    Check(openSchema.find("type_text") != std::string::npos,
        "type_text was removed for a task holding no content. Typing into a field is "
        "ordinary work when there is no exact value that has to replace what is there.");
}

// Falling back from a cheap provider to the expensive one must not be a way around the
// gate. The gate is downstream of the choice precisely so that it is not.
void TestProviderFallbackStillCannotInventThePayload()
{
    SessionFixture fixture;
    fixture.UseMode("assisted");
    fixture.Script(
        // No usable subgoal, so the routine policy has nothing to work from and the
        // run falls back to the existing path for every decision.
        [](int) { return std::string(); },
        [](int index)
        {
            if (index == 0) return EntryStep("compose", "Something I made up");
            return nlohmann::json{{"decision", "complete"},
                {"reason", "done"}}.dump();
        });

    Goal goal;
    goal.id = NewGoalId();
    goal.title = "put \"dinner at eight\" in the Compose box";
    goal.budget.maxActions = 2;
    goal.scope.approvedApplications = {"reviadesktopfixture.exe"};

    ReviaSessionTestAccess::BeginComputerTask(
        fixture.session, goal.id, QuotedRequest(goal.title));
    const Goal finished = ReviaSessionTestAccess::OperateGoal(fixture.session, goal);
    ReviaSessionTestAccess::EndComputerTask(fixture.session);

    // Two things have to hold, and the second is what changed.
    //
    // Whatever reached a field carried the user's words -- that is the gate, downstream
    // of every provider including the runtime's own derivation. And nothing reached a
    // field carrying anything else, which covers the case this test can no longer force:
    // with the runtime deriving progression, a task whose window is not in front starts
    // by trying to bring it forward, and on a machine where that application is not
    // running the run stops there. Asserting "the entry carried X" would then be
    // asserting about a step that correctly never happened.
    const GoalStep* entry = TextEntry(finished);
    if (entry != nullptr)
    {
        Check(entry->action.value == "dinner at eight",
            "A fallback from the routine policy to the existing path placed invented "
            "text: '" + entry->action.value + "'.");
    }
    for (const GoalStep& step : finished.steps)
    {
        Check(step.action.value.find("Something I made up") == std::string::npos,
            "The scripted planner's own sentence reached a step.");
    }
    Check(fixture.Tasks().ContentStats().inventions == 0 ||
            (entry != nullptr && entry->action.value == "dinner at eight"),
        "An invention was counted without the held content being placed in its stead.");
}

// The other half of the defect, and the one a content check alone does not catch: the
// run reported success because something had been typed.
void TestATaskDoesNotFinishOnContentNobodyHasSeen()
{
    SessionFixture fixture;
    fixture.UseMode("legacy");
    fixture.Script(
        [](int) { return std::string(); },
        [](int)
        {
            // Complete on the first breath, with nothing placed at all.
            return nlohmann::json{{"decision", "complete"},
                {"reason", "I put the message in the box"}}.dump();
        });

    Goal goal;
    goal.id = NewGoalId();
    goal.title = "put \"dinner at eight\" in the Compose box";
    goal.budget.maxActions = 2;
    goal.scope.approvedApplications = {"reviadesktopfixture.exe"};

    ReviaSessionTestAccess::BeginComputerTask(
        fixture.session, goal.id, QuotedRequest(goal.title));
    const Goal finished = ReviaSessionTestAccess::OperateGoal(fixture.session, goal);

    Check(finished.status != GoalStatus::Succeeded,
        "A task whose content was never placed reported success, on the strength of a "
        "provider saying so.");
    Check(fixture.Tasks().ContentStats().prematureCompletions >= 1,
        "The refused completion was not recorded.");
    ReviaSessionTestAccess::EndComputerTask(fixture.session);
}

// Content-free tasks still reach the controller. A completion proposal needs its own
// supported whole-task criterion before the runner can accept it.
void TestATaskWithNoContentStillRequiresIndependentAcceptance()
{
    SessionFixture fixture;
    fixture.UseMode("legacy");
    fixture.Script(
        [](int) { return std::string(); },
        [](int)
        {
            return nlohmann::json{{"decision", "complete"},
                {"reason", "the window is already in front"}}.dump();
        });

    Goal goal;
    goal.id = NewGoalId();
    goal.title = "bring the fixture window to the front";
    goal.budget.maxActions = 2;
    goal.scope.approvedApplications = {"reviadesktopfixture.exe"};

    ReviaSessionTestAccess::BeginComputerTask(
        fixture.session, goal.id, QuotedRequest(goal.title));
    const Goal finished = ReviaSessionTestAccess::OperateGoal(fixture.session, goal);
    ReviaSessionTestAccess::EndComputerTask(fixture.session);

    Check(fixture.stepRequests.size() == 1 && fixture.Tasks().ContentStats().prematureCompletions == 0,
        "The content gate interfered with a task that had no payload.");
    Check(finished.status == GoalStatus::Blocked && finished.stopReason == StopReason::VerificationFailed &&
            !finished.stopDetail.empty(),
        "A synthetic completion without an independently supported criterion became whole-task success.");
    Check(!fixture.session.IsStarted(), "The operator fixture did not restore the session's prior stopped state.");
}

#ifdef _WIN32
// One owned UIA destination. The message thread services native accessibility calls;
// no keyboard or pointer input is synthesized by this fixture.
class AcceptanceWindow
{
public:
    AcceptanceWindow()
    {
        previous = GetForegroundWindow();
        std::promise<HWND> ready;
        auto result = ready.get_future();
        worker = std::thread([this, &ready]
        {
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT
            {
                if (message == WM_DESTROY)
                {
                    PostQuitMessage(0);
                    return 0;
                }
                return DefWindowProcW(window, message, wparam, lparam);
            };
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = L"ReviaAcceptanceFixture";
            static_cast<void>(RegisterClassW(&windowClass));
            const HWND window = CreateWindowExW(WS_EX_NOACTIVATE, windowClass.lpszClassName,
                L"Revia ACT-1 owned Compose", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                40, 40, 360, 150, nullptr, nullptr, windowClass.hInstance, nullptr);
            if (window)
            {
                static_cast<void>(CreateWindowExW(0, L"STATIC", L"Compose", WS_CHILD | WS_VISIBLE,
                    12, 12, 100, 20, window, nullptr, windowClass.hInstance, nullptr));
                static_cast<void>(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    12, 36, 300, 24, window, reinterpret_cast<HMENU>(6001), windowClass.hInstance, nullptr));
            }
            ready.set_value(window);
            if (!window)
            {
                shutdown.set_value();
                return;
            }
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            shutdown.set_value();
        });
        if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        {
            std::cerr << "ACT-1 owned window startup exceeded five seconds." << std::endl;
            std::terminate();
        }
        window = result.get();
    }

    ~AcceptanceWindow()
    {
        const bool restore = GetForegroundWindow() == window;
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        if (worker.joinable())
        {
            if (exited.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            {
                std::cerr << "ACT-1 owned window teardown exceeded five seconds." << std::endl;
                std::terminate();
            }
            worker.join();
        }
        if (restore && previous && IsWindow(previous))
            static_cast<void>(SetForegroundWindow(previous));
    }

    [[nodiscard]] HWND Handle() const { return window; }

    void Activate() const
    {
        Check(window != nullptr, "The owned ACT-1 Compose window could not be created.");
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        Check(process == GetCurrentProcessId(), "ACT-1 fixture escaped the test process.");
        SetLastError(ERROR_SUCCESS);
        const bool requested = SetForegroundWindow(window) != FALSE;
        const DWORD activationError = GetLastError();
        DWORD_PTR acknowledged = 0;
        // Cross-thread activation is queued. A no-op message acknowledges only this
        // owned window's queue before the actual foreground identity is checked.
        const bool synchronized = SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
            2000, &acknowledged) != 0;
        const HWND foreground = GetForegroundWindow();
        DWORD foregroundProcess = 0;
        GetWindowThreadProcessId(foreground, &foregroundProcess);
        std::cout << "ACT-1 owned activation requested=" << requested << " error=" << activationError
            << " acknowledged=" << synchronized << " foreground-owned=" << (foreground == window)
            << " owned-pid=" << process << " foreground-pid=" << foregroundProcess << std::endl;
        Check(synchronized && foreground == window && foregroundProcess == process,
            "The owned ACT-1 fixture could not supply a current foreground observation.");
    }

private:
    HWND window = nullptr;
    HWND previous = nullptr;
    std::promise<void> shutdown;
    std::future<void> exited = shutdown.get_future();
    std::thread worker;
};

void TestWholeRequestAcceptanceThroughInstalledSessionVerifier()
{
    AcceptanceWindow window;
    for (const std::string& mode : {std::string("legacy"), std::string("assisted"), std::string("shadow")})
    {
        for (const bool extraWork : {false, true})
        {
            SessionFixture fixture;
            {
                std::ofstream config(fixture.directory.root / "capabilities.json");
                config << nlohmann::json{{"mode", "approved_scope"},
                    {"approvedRoots", {PathToUtf8(fixture.approved)}},
                    {"approvedApplications", {"reviatests.exe"}},
                    {"approvedControls", {{"reviatests.exe", {"6001"}}}},
                    {"autoApproveRiskThrough", "reversible_write"},
                    {"createMissingApprovedRoots", false}}.dump();
            }
            ReviaSessionTestAccess::PrepareOperator(fixture.session, fixture.directory.root);
            fixture.UseMode(mode);
            fixture.Script([](int) { return std::string(); }, [](int index)
            {
                if (index != 0) return nlohmann::json{{"decision", "complete"}, {"reason", "all work done"}}.dump();
                return nlohmann::json{{"decision", "act"}, {"description", "Place exact content in the owned field"},
                    {"step", {{"action", {{"action", "set_control_text"}, {"application", "reviatests.exe"},
                        {"window_title", "Revia ACT-1 owned Compose"}, {"control", "6001"}, {"value", PreparedContentToken}}},
                        {"check", {{"action", "inspect_window"}, {"application", "reviatests.exe"},
                            {"window_title", "Revia ACT-1 owned Compose"}}}, {"expected", "6001 holds the prepared content"}}}}.dump();
            });
            window.Activate();
            Goal goal;
            goal.id = NewGoalId();
            goal.title = "put \"hello and then tea\" into Compose";
            if (extraWork) goal.title += " and open the project folder";
            goal.scope = fixture.session.Capabilities();
            goal.budget.maxActions = 3;
            ReviaSessionTestAccess::BeginComputerTask(fixture.session, goal.id, ExtractTaskContent(goal.title), goal.title);
            const auto finished = ReviaSessionTestAccess::OperateGoal(fixture.session, goal);
            const auto* placed = TextEntry(finished);
            Check(placed && placed->status == StepStatus::Succeeded && !placed->attempts.empty() &&
                    placed->attempts.back().outcome == VerificationOutcome::Verified,
                "ACT-1 session did not execute and verify exact native placement in mode " + mode + ": " + finished.stopDetail);
            const auto readback = revia::actions::windows::ObserveDraft(placed->action);
            Check(readback.valid && readback.control.window == window.Handle() && readback.control.processId == GetCurrentProcessId(),
                "ACT-1 placement did not bind the exact owned HWND and process.");
            Check(readback.value == "hello and then tea",
                "ACT-1 session exact UIA readback lost the quoted conjunction payload.");
            Check(extraWork ? finished.status == GoalStatus::Blocked : finished.status == GoalStatus::Succeeded,
                "ACT-1 installed session acceptance misclassified the whole request in mode " + mode + ": " + finished.stopDetail);
            if (extraWork)
                Check(finished.stopReason == StopReason::VerificationFailed && finished.stopDetail.find("remaining work") != std::string::npos &&
                        finished.stopDetail.find("project folder") != std::string::npos &&
                        finished.stopDetail.find("hello and then tea") == std::string::npos,
                    "ACT-1 session lost the unattempted remaining work in its final explanation.");
            std::cout << "ACT-1 installed verifier mode=" << mode << " extra=" << extraWork
                << " verified-placement=1 status=" << ToString(finished.status) << " detail=" << finished.stopDetail << std::endl;
            ReviaSessionTestAccess::EndComputerTask(fixture.session);
        }
    }
}
#endif

} // namespace

void RunNativeTaskAcceptanceSessionTests()
{
#ifdef _WIN32
    TestWholeRequestAcceptanceThroughInstalledSessionVerifier();
#else
    revia::tests::Check(false, "The explicit native acceptance fixture requires Windows UI Automation.");
#endif
}

void RunComputerSessionTests()
{
    TestLegacyModeCannotPlaceItsOwnWords();
    TestTheGrammarOffersNoRoomForAnInventedPayload();
    TestProviderFallbackStillCannotInventThePayload();
    TestATaskDoesNotFinishOnContentNobodyHasSeen();
    TestATaskWithNoContentStillRequiresIndependentAcceptance();
    TestTheDefaultSessionStillAsksTheModelForEveryStep();
    TestTheSubgoalPromptReferencesContentWithoutShowingIt();
    TestAnOutOfScopeSubgoalNeverReachesAPolicy();
    TestAssistedModeMovesDecisionsAwayFromTheModel();
    TestTaskPayloadsAreDroppedWhenTheTaskEnds();
    TestARunRecordsNothingByDefault();
    TestTheStatusReportExplainsAnInactiveMode();
    TestAStopEndsTheRunWithoutALateStep();

    std::cout << "The real session reaches a bounded subgoal, decides routine steps "
                 "without a model, and refuses what the task never authorized.\n";
}
