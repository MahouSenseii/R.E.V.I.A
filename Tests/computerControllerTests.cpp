#include "testSupport.h"

#include "Computer/computerController.h"
#include "Computer/legacyLlmPolicy.h"

#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace revia::computer;
using revia::tests::Check;

// The decision seam, exercised without a model, a desktop or a session.
//
// These tests are about the seam itself: that one look is shared rather than taken per
// provider, that a decision vocabulary wider than NextStep collapses onto it honestly,
// and that a policy proposes without being able to stamp anything the runtime owns.

revia::actions::windows::DesktopObservation MakeScreen()
{
    revia::actions::windows::DesktopObservation screen;
    screen.succeeded = true;
    screen.generation = 41;
    screen.id = "observation-41";
    screen.foregroundApplication = "notepad.exe";
    screen.foregroundTitle = "Untitled - Notepad";
    screen.windowLeft = 10;
    screen.windowTop = 20;
    screen.windowRight = 810;
    screen.windowBottom = 620;

    revia::actions::windows::ObservedControl edit;
    edit.name = "Text editor";
    edit.automationId = "15";
    edit.enabled = true;
    edit.editable = true;
    edit.left = 12;
    edit.top = 40;
    edit.right = 800;
    edit.bottom = 600;
    screen.controls.push_back(edit);

    revia::actions::windows::ObservedControl save;
    save.name = "Save";
    save.enabled = true;
    save.invokable = true;
    save.left = 700;
    save.top = 4;
    save.right = 760;
    save.bottom = 28;
    screen.controls.push_back(save);
    return screen;
}

ComputerTaskContext MakeContext()
{
    ComputerTaskContext context;
    context.subgoal = "Save the note";
    context.iteration = 2;
    context.stepsTaken = 1;
    context.actionsLeft = 17;
    context.retriesLeft = 6;
    context.observation.screen = MakeScreen();
    context.observation.changedSinceLastDecision = true;

    ObservedCandidate editor;
    editor.id = "15";
    editor.name = "Text editor";
    editor.mayType = true;
    editor.maySetText = true;
    context.observation.candidates.push_back(editor);

    ObservedCandidate save;
    save.id = "Save";
    save.name = "Save";
    save.mayInvoke = true;
    context.observation.candidates.push_back(save);

    ComputerAttempt attempt;
    attempt.description = "Type the note";
    attempt.action = revia::actions::ActionType::TypeText;
    attempt.expected = "hello";
    attempt.status = revia::goals::StepStatus::Succeeded;
    attempt.executed = true;
    attempt.verified = true;
    attempt.observed = "Untitled - Notepad | hello";
    context.recentAttempts.push_back(attempt);
    return context;
}

// A policy that answers whatever it is told to, and records what it was asked.
class ScriptedPolicy final : public IComputerPolicy
{
public:
    explicit ScriptedPolicy(ComputerDecision answer) : reply(std::move(answer)) {}

    [[nodiscard]] std::string Name() const override { return "scripted"; }
    [[nodiscard]] bool IsAvailable() const override { return available; }

    [[nodiscard]] ComputerDecision Decide(
        const ComputerTaskContext& context, std::stop_token) override
    {
        ++calls;
        seenGeneration = context.observation.screen.generation;
        return reply;
    }

    ComputerDecision reply;
    bool available = true;
    int calls = 0;
    std::uint64_t seenGeneration = 0;
};

// The formatter is the compatibility layer: the shape it emits is what the current
// model is prompted with, and changing that shape silently would change behaviour
// while every test still passed.
void TestTheLegacyContextKeepsItsShape()
{
    const nlohmann::json formatted = nlohmann::json::parse(FormatLegacyContext(MakeContext()));

    for (const char* key : {"goal", "iteration", "observation", "steps_taken",
            "actions_left", "retries_left", "scope", "history"})
    {
        Check(formatted.contains(key),
            std::string("The decision context lost its \"") + key + "\" field.");
    }
    Check(formatted["goal"] == "Save the note" && formatted["iteration"] == 2 &&
        formatted["actions_left"] == 17 && formatted["retries_left"] == 6,
        "The decision context misreported the goal or what is left of its budget.");

    const nlohmann::json& observation = formatted["observation"];
    Check(observation["available"] == true &&
        observation["application"] == "notepad.exe" &&
        observation["title"] == "Untitled - Notepad",
        "The decision context lost the window it was looking at.");
    Check(observation.contains("controls") && observation["controls"].is_string() &&
        observation["controls"].get<std::string>().find("notepad.exe") != std::string::npos,
        "The decision context lost the described controls.");
    Check(observation["screen_changed_since_last_decision"] == true,
        "The decision context lost whether anything had moved since the last decision.");

    // Only the operations the candidate actually advertises and policy allows, split
    // the way the model is told to read them.
    const nlohmann::json& targets = observation["control_targets"];
    Check(targets["invoke_control"] == nlohmann::json::array({"Save"}),
        "An invokable target was lost or an uninvokable one was offered.");
    Check(targets["type_text"] == nlohmann::json::array({"15"}) &&
        targets["set_control_text"] == nlohmann::json::array({"15"}),
        "An editable target was lost or a non-editable one was offered.");

    Check(formatted["history"].size() == 1 &&
        formatted["history"][0]["observed"] == "Untitled - Notepad | hello" &&
        formatted["history"][0]["verified"] == true,
        "The decision context lost what the last check actually saw.");
}

// A withheld window has to read as "I cannot see here", not as "there is nothing here".
// Those lead to opposite next moves.
void TestAWithheldWindowIsNotAnEmptyScreen()
{
    ComputerTaskContext withheld = MakeContext();
    withheld.observation.withheld = true;
    const nlohmann::json formatted = nlohmann::json::parse(FormatLegacyContext(withheld));
    Check(formatted["observation"]["available"] == false &&
        formatted["observation"]["reason"].get<std::string>().find("excluded") !=
            std::string::npos,
        "An excluded window was not reported as withheld.");
    Check(formatted["observation"]["control_targets"]["invoke_control"].empty(),
        "An excluded window still offered targets to act on.");

    ComputerTaskContext blind = MakeContext();
    blind.observation.screen = revia::actions::windows::DesktopObservation{};
    blind.observation.screen.failure = "Windows UI Automation is unavailable.";
    const nlohmann::json unseen = nlohmann::json::parse(FormatLegacyContext(blind));
    Check(unseen["observation"]["available"] == false &&
        unseen["observation"]["reason"] == "Windows UI Automation is unavailable.",
        "A failed observation lost the reason it failed.");
}

// The first iteration has nothing to have changed since, and saying "nothing changed"
// there would be a claim about a screen that was never compared.
void TestTheFirstDecisionClaimsNoChange()
{
    ComputerTaskContext first = MakeContext();
    first.iteration = 0;
    first.observation.changedSinceLastDecision.reset();
    const nlohmann::json formatted = nlohmann::json::parse(FormatLegacyContext(first));
    Check(!formatted["observation"].contains("screen_changed_since_last_decision"),
        "The first decision was told whether the screen had changed since nothing.");
}

// NextStep is three-valued on purpose. A wider decision vocabulary has to collapse onto
// it without turning "stuck" into "finished" or a refusal into a crash.
void TestEveryDecisionMapsOntoTheRunnersVocabulary()
{
    const auto decide = [](const ComputerDecisionKind kind, const std::string& detail)
    {
        ComputerDecision answer;
        answer.kind = kind;
        answer.detail = detail;
        if (kind == ComputerDecisionKind::ProposeAction)
        {
            answer.step.description = "Press save";
            answer.step.action.type = revia::actions::ActionType::InvokeControl;
            answer.step.action.application = "notepad.exe";
            answer.step.action.control = "Save";
            answer.step.check.type = revia::actions::ActionType::InspectWindow;
            answer.step.check.application = "notepad.exe";
            answer.step.expected = "Saved";
        }
        PayloadVault vault;
        ComputerController controller{vault};
        controller.SetLegacyPolicy(std::make_unique<ScriptedPolicy>(answer));
        return controller.Decide(MakeContext(), {});
    };

    const auto acting = decide(ComputerDecisionKind::ProposeAction, {});
    Check(acting.hasStep && !acting.finished &&
        acting.step.action.type == revia::actions::ActionType::InvokeControl,
        "A proposed action did not reach the runner as a step.");
    // The provider proposes; the runtime stamps. An ordinal or a requested-by a
    // provider chose for itself would be a claim about the record it does not own.
    Check(acting.step.ordinal == 0 && acting.step.action.requestedBy != "goal",
        "The controller stamped fields that belong to the runtime.");

    const auto complete = decide(ComputerDecisionKind::ProposeCompletion, "The title says saved.");
    Check(!complete.hasStep && complete.finished && complete.reason == "The title says saved.",
        "A proposed completion did not reach the runner as finished.");

    for (const auto kind : {ComputerDecisionKind::NeedReasoning,
            ComputerDecisionKind::NeedVision, ComputerDecisionKind::NeedUser,
            ComputerDecisionKind::WaitForState, ComputerDecisionKind::Reobserve,
            ComputerDecisionKind::CannotHandle})
    {
        const auto stuck = decide(kind, "beyond me");
        Check(!stuck.hasStep && !stuck.finished && stuck.reason == "beyond me",
            "A decision that was neither a step nor a completion was collapsed into one.");
    }
}

// An unavailable provider is not a provider that keeps answering badly.
void TestAnAbsentProviderRefusesRatherThanGuessing()
{
    PayloadVault emptyVault;
    ComputerController empty{emptyVault};
    const auto none = empty.Decide(MakeContext(), {});
    Check(!none.hasStep && !none.finished && !none.reason.empty(),
        "A controller with no policy claimed a step or a completion.");

    ComputerDecision unused;
    unused.kind = ComputerDecisionKind::ProposeCompletion;
    auto offline = std::make_unique<ScriptedPolicy>(unused);
    offline->available = false;
    ScriptedPolicy& watched = *offline;
    PayloadVault vault;
    ComputerController controller{vault};
    controller.SetLegacyPolicy(std::move(offline));
    const auto refused = controller.Decide(MakeContext(), {});
    Check(!refused.finished && watched.calls == 0,
        "An unavailable policy was asked anyway, and its answer was believed.");
    Check(controller.LastDecision().code == ComputerReasonCode::ProviderUnavailable,
        "An unavailable provider was not distinguished from one that had no answer.");
}

// Every provider consulted for an iteration reasons about the same look. Taking a
// second one would bump the process-wide observation generation and invalidate a visual
// target another provider had already chosen correctly.
void TestEveryProviderSeesTheSameLook()
{
    const std::uint64_t before = revia::actions::windows::DesktopObserver::LatestGeneration();

    const ComputerTaskContext context = MakeContext();
    ComputerDecision nothing;
    auto first = std::make_unique<ScriptedPolicy>(nothing);
    auto second = std::make_unique<ScriptedPolicy>(nothing);
    ScriptedPolicy& watchedFirst = *first;
    ScriptedPolicy& watchedSecond = *second;

    PayloadVault oneVault;
    ComputerController one{oneVault};
    one.SetLegacyPolicy(std::move(first));
    PayloadVault twoVault;
    ComputerController two{twoVault};
    two.SetLegacyPolicy(std::move(second));
    static_cast<void>(one.Decide(context, {}));
    static_cast<void>(two.Decide(context, {}));

    Check(watchedFirst.seenGeneration == watchedSecond.seenGeneration &&
        watchedFirst.seenGeneration == context.observation.screen.generation,
        "Two providers reasoned about different screens.");
    Check(revia::actions::windows::DesktopObserver::LatestGeneration() == before,
        "Consulting a policy took an observation of its own.");
}

// A policy failure is data, not an exception, and it must not look like a completion.
void TestAFailedPlannerCallIsNotACompletion()
{
    LegacyLlmComputerPolicy failing([](const std::string&, std::stop_token)
    {
        responseOutput output;
        output.bSuccess = false;
        output.reason = "The model was unreachable.";
        return output;
    });
    const ComputerDecision decision = failing.Decide(MakeContext(), {});
    Check(decision.kind == ComputerDecisionKind::CannotHandle &&
        decision.code == ComputerReasonCode::ProviderFailed &&
        decision.detail == "The model was unreachable.",
        "A failed planner call was not reported as one.");

    LegacyLlmComputerPolicy malformed([](const std::string&, std::stop_token)
    {
        responseOutput output;
        output.bSuccess = true;
        output.response = "{not json";
        return output;
    });
    const ComputerDecision garbage = malformed.Decide(MakeContext(), {});
    Check(garbage.kind == ComputerDecisionKind::CannotHandle &&
        garbage.code == ComputerReasonCode::MalformedProviderOutput,
        "Malformed provider output was not rejected as malformed.");

    LegacyLlmComputerPolicy finished([](const std::string&, std::stop_token)
    {
        responseOutput output;
        output.bSuccess = true;
        output.response = R"({"decision":"complete","reason":"The note is saved."})";
        return output;
    });
    const ComputerDecision done = finished.Decide(MakeContext(), {});
    Check(done.kind == ComputerDecisionKind::ProposeCompletion &&
        done.detail == "The note is saved.",
        "An explicit completion was not recognised.");
    Check(done.provider == "legacy_llm", "A decision was not attributed to its provider.");
}

} // namespace

void RunComputerControllerTests()
{
    TestTheLegacyContextKeepsItsShape();
    TestAWithheldWindowIsNotAnEmptyScreen();
    TestTheFirstDecisionClaimsNoChange();
    TestEveryDecisionMapsOntoTheRunnersVocabulary();
    TestAnAbsentProviderRefusesRatherThanGuessing();
    TestEveryProviderSeesTheSameLook();
    TestAFailedPlannerCallIsNotACompletion();
    std::cout << "Computer controller tests passed: one look, one decision, and the "
                 "provider proposes rather than stamps.\n";
}
