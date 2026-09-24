#include "testSupport.h"

#include "Computer/observationBuilder.h"
#include "Computer/routinePolicy.h"
#include "Computer/subgoalValidator.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace revia::computer;
using revia::actions::windows::DesktopObservation;
using revia::actions::windows::ObservedControl;
using revia::tests::Check;

// ISSUE-REVIA-0070, from both directions.
//
// The defect was that DesktopObserver dropped every element with an empty accessible
// name, so an unlabelled input was invisible to every decision and the only way to reach
// one was a raw coordinate. Admitting them is the fix; admitting them *safely* is the
// work, because a nameless control is also a control nothing can describe.
//
// So these tests come in pairs. A nameless field that context identifies must be
// reachable, and a nameless field that context does not identify must escalate. A fix
// that only satisfied the first half would have replaced an invisible control with a
// guessed one.

revia::actions::CapabilitySettings Scope()
{
    revia::actions::CapabilitySettings scope;
    scope.approvedApplications = {"fixture.exe"};
    // Without this every candidate is refused before it is built -- the capability
    // policy requires an approved control scope per application, and a test scope that
    // omitted it would make BuildCandidates return nothing and look like the defect.
    scope.approvedControls = {{"fixture.exe", {"*"}}};
    scope.desktopControl.keyboard = true;
    scope.desktopControl.pointer = true;
    scope.desktopControl.maxTypedCharacters = 512;
    return scope;
}

ObservedControl Named(const std::string& name, const std::string& id, const int type)
{
    ObservedControl control;
    control.name = name;
    control.automationId = id;
    control.controlType = type;
    control.enabled = true;
    control.invokable = type == 50000;
    control.editable = type == 50004;
    return control;
}

ObservedControl Nameless(
    const std::string& id,
    const int type,
    const std::string& container = {},
    const std::string& inferredLabel = {})
{
    ObservedControl control;
    control.automationId = id;
    control.controlType = type;
    control.enabled = true;
    control.nameless = true;
    control.containerName = container;
    control.inferredLabel = inferredLabel;
    control.invokable = type == 50000;
    control.editable = type == 50004;
    return control;
}

DesktopObservation Screen(std::vector<ObservedControl> controls)
{
    DesktopObservation screen;
    screen.succeeded = true;
    screen.foregroundApplication = "fixture.exe";
    screen.controls = std::move(controls);
    return screen;
}

ComputerSubgoal Validated(
    const SubgoalIntent intent,
    const std::string& name,
    const std::string& role,
    const std::string& container,
    const PayloadVault& vault,
    const PayloadReference& payload = {})
{
    ComputerSubgoal proposed;
    proposed.id = NewSubgoalId();
    proposed.intent = intent;
    proposed.target.application = "fixture.exe";
    proposed.target.name = name;
    proposed.target.role = role;
    proposed.target.container = container;
    proposed.payload = payload;

    SubgoalContext context;
    context.goalId = "goal-1";
    context.origin = RequestOrigin::UserDirected;
    context.scope = Scope();

    const SubgoalValidation validation = ValidateSubgoal(proposed, context, vault);
    Check(validation.accepted, "The test's own subgoal was refused: " + validation.detail);
    return validation.subgoal;
}

ComputerTaskContext Context(const DesktopObservation& screen)
{
    ComputerTaskContext context;
    context.observation.screen = screen;
    context.observation.candidates = BuildCandidates(screen, Scope());
    context.scope = Scope();
    return context;
}

// The defect itself: an unlabelled edit control used to be absent entirely.
void TestANamelessControlIsNoLongerDiscarded()
{
    const DesktopObservation screen = Screen({
        Named("Save", "1005", 50000),
        Nameless("1002", 50004),
    });
    const auto candidates = BuildCandidates(screen, Scope());

    Check(candidates.size() == 2,
        "A nameless but editable control was dropped before any decision saw it, which "
        "is ISSUE-REVIA-0070; " + std::to_string(candidates.size()) +
            " candidate(s) came back.");
    const auto nameless = std::find_if(candidates.begin(), candidates.end(),
        [](const ObservedCandidate& candidate) { return candidate.nameless; });
    Check(nameless != candidates.end() && nameless->id == "1002" &&
            (nameless->maySetText || nameless->mayType),
        "The nameless control came back without the thing that makes it useful.");
}

// Admitting them must not admit everything.
void TestOnlyUsefulNamelessControlsAreAdmitted()
{
    ObservedControl decoration = Nameless("panel-1", 50033);
    decoration.invokable = false;
    decoration.editable = false;
    ObservedControl unreferable = Nameless("", 50004);
    ObservedControl disabled = Nameless("1009", 50004);
    disabled.enabled = false;

    const auto candidates = BuildCandidates(
        Screen({decoration, unreferable, disabled, Nameless("1002", 50004)}), Scope());
    Check(candidates.size() == 1 && candidates[0].id == "1002",
        "Admitting nameless controls admitted the structural scenery with them; " +
            std::to_string(candidates.size()) + " came back.");
}

// The one control that never becomes a candidate, and the reason this check had to be
// added at the same time as the fix.
void TestANamelessPasswordFieldIsNeverACandidate()
{
    ObservedControl password = Nameless("1004", 50004);
    password.isPassword = true;

    const auto candidates = BuildCandidates(
        Screen({password, Nameless("1002", 50004)}), Scope());
    Check(candidates.size() == 1 && candidates[0].id == "1002",
        "A nameless password box became an ordinary typeable candidate. It was "
        "previously protected by being invisible, and by the consequence classifier "
        "reading a name it does not have -- both of which the fix removes.");

    // And a named one is still refused, so the new check did not replace the old one.
    ObservedControl namedPassword = Named("Password", "1004", 50004);
    namedPassword.isPassword = true;
    const auto named = BuildCandidates(Screen({namedPassword}), Scope());
    Check(named.empty(), "A named password box became a candidate.");
}

// Context resolves what a name cannot.
void TestAContainerIdentifiesANamelessField()
{
    PayloadVault vault;
    const PayloadReference payload = vault.Store("dinner at eight", "message");
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(Validated(
        SubgoalIntent::EnterPayload, "", "edit", "Compose", vault, payload));

    const ComputerDecision decision = policy.Decide(Context(Screen({
        Nameless("1017", 50004, "Compose"),
        Nameless("1018", 50004, "Subject"),
    })), {});

    Check(decision.kind == ComputerDecisionKind::ProposeAction &&
            decision.step.action.control == "1017",
        "Two nameless fields in differently named panels were not told apart by the "
        "panel, which is the only thing that distinguishes them.");
}

// And where context does not resolve, the answer is escalation rather than a guess.
void TestIndistinguishableNamelessFieldsEscalate()
{
    PayloadVault vault;
    const PayloadReference payload = vault.Store("dinner at eight", "message");
    RoutineComputerPolicy policy(vault);

    // Two nameless edits, no label, no container, nothing to choose between them. This
    // is the main fixture window, and the honest answer is that it cannot be done.
    policy.SetSubgoal(Validated(
        SubgoalIntent::EnterPayload, "Document", "edit", "", vault, payload));
    const ComputerDecision byName = policy.Decide(Context(Screen({
        Nameless("1002", 50004),
        Nameless("1003", 50004),
    })), {});
    Check(byName.kind != ComputerDecisionKind::ProposeAction,
        "A subgoal naming a control that nothing on screen publishes was acted on "
        "anyway; the policy picked one of two indistinguishable fields.");

    // Naming the container does not help either when both share it.
    policy.SetSubgoal(Validated(
        SubgoalIntent::EnterPayload, "", "edit", "Compose", vault, payload));
    const ComputerDecision byContainer = policy.Decide(Context(Screen({
        Nameless("1017", 50004, "Compose"),
        Nameless("1019", 50004, "Compose"),
    })), {});
    Check(byContainer.kind == ComputerDecisionKind::NeedUser,
        "Two nameless fields in the same panel were not treated as a question for a "
        "person; the decision was " + ToString(byContainer.kind) + ".");
}

// A published name outranks an inferred one. They are different kinds of evidence and
// the weaker must not outvote the stronger.
void TestAPublishedNameBeatsAnInferredLabel()
{
    PayloadVault vault;
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(Validated(
        SubgoalIntent::InteractWithControl, "Send", "button", "", vault));

    // One control says it is called Send. Another says nothing, and UI Automation
    // reasons that a nearby static labels it Send.
    ObservedControl published = Named("Send", "1006", 50000);
    ObservedControl inferred = Nameless("1020", 50000, "", "Send");

    const ComputerDecision decision =
        policy.Decide(Context(Screen({inferred, published})), {});
    Check(decision.kind == ComputerDecisionKind::ProposeAction &&
            decision.step.action.control == "1006",
        "An inferred label was allowed to compete with a name the application actually "
        "published, and the guess won.");
}

// An inferred label still resolves when nothing published anything.
void TestAnInferredLabelResolvesWhenNothingElseDoes()
{
    PayloadVault vault;
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(Validated(
        SubgoalIntent::InteractWithControl, "Send", "button", "", vault));

    const ComputerDecision decision = policy.Decide(Context(Screen({
        Nameless("1020", 50000, "", "Send"),
        Nameless("1021", 50000, "", "Cancel"),
    })), {});
    Check(decision.kind == ComputerDecisionKind::ProposeAction &&
            decision.step.action.control == "1020",
        "A control whose only identification is UI Automation's own label relationship "
        "was not reachable.");
}

// Named controls keep their place in the budget. The fix must only ever add candidates.
void TestNamedControlsAreNotCrowdedOut()
{
    std::vector<ObservedControl> controls;
    // Far more nameless scenery than the cap allows, with the named control last.
    for (int index = 0; index < 60; ++index)
    {
        controls.push_back(Nameless("filler-" + std::to_string(index), 50004));
    }
    controls.push_back(Named("Save", "1005", 50000));

    // The observer admits named controls first, so by the time this reaches
    // BuildCandidates the named one is already at the front. Asserted here against the
    // observation the observer would produce, with the cap doing its job.
    std::vector<ObservedControl> asObserved;
    asObserved.push_back(controls.back());
    asObserved.insert(asObserved.end(), controls.begin(), controls.end() - 1);

    const auto candidates = BuildCandidates(Screen(asObserved), Scope(), 40);
    Check(!candidates.empty() && candidates[0].name == "Save",
        "The named control lost its place to nameless scenery.");
    Check(candidates.size() <= 40,
        "Admitting nameless controls broke the bound on how many are listed; " +
            std::to_string(candidates.size()) + " came back.");
}

} // namespace

void RunNamelessControlTests()
{
    TestANamelessControlIsNoLongerDiscarded();
    TestOnlyUsefulNamelessControlsAreAdmitted();
    TestANamelessPasswordFieldIsNeverACandidate();
    TestAContainerIdentifiesANamelessField();
    TestIndistinguishableNamelessFieldsEscalate();
    TestAPublishedNameBeatsAnInferredLabel();
    TestAnInferredLabelResolvesWhenNothingElseDoes();
    TestNamedControlsAreNotCrowdedOut();

    std::cout << "A control with no name is reachable when context identifies it and "
                 "escalates when nothing does.\n";
}
