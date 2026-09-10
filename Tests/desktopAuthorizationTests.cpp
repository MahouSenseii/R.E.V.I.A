#include "testSupport.h"

#include "Policy/desktopAuthorization.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace revia::policy;
using revia::actions::ConsequenceClass;
using revia::tests::Check;

// The shared authorizer, tested without a desktop.
//
// Everything here is a pure function of a typed request, which is the point of putting
// the decision in Policy rather than in each executor: the rules can be exercised
// exhaustively, and there is only one copy of them to exercise.
//
// These tests are about what is refused and why. They are evidence for the cases below
// and not proof that desktop interaction is safe -- the authorizer runs inside the
// process it protects, and a check made immediately before SendInput is still a
// check-to-use race.

revia::actions::CapabilitySettings::DesktopControl Ceiling(const ConsequenceClass ceiling)
{
    revia::actions::CapabilitySettings::DesktopControl settings;
    settings.pointer = true;
    settings.keyboard = true;
    settings.maxUnconfirmedConsequence = ceiling;
    return settings;
}

TargetEvidence Control(const std::string& name, const std::string& window = {})
{
    TargetEvidence evidence;
    evidence.resolved = true;
    evidence.controlName = name;
    evidence.windowTitle = window;
    evidence.executable = "notepad.exe";
    return evidence;
}

AuthorizationDecision Decide(
    const DesktopOperation operation,
    const TargetEvidence& evidence,
    const ConsequenceClass ceiling,
    const bool autonomous = true,
    const bool scratch = false)
{
    AuthorizationRequest request;
    request.operation = operation;
    request.evidence = evidence;
    request.autonomousOrigin = autonomous;
    request.insideApprovedScratch = scratch;
    return AuthorizeDesktopEffect(request, Ceiling(ceiling));
}

void TestTheSameEffectCostsTheSameWhicheverRouteReachesIt()
{
    // The defect this whole component exists for: clicking Send was gated, and invoking
    // the same button through UI Automation was not. A consequence is a property of the
    // target, not of the mechanism that reached it.
    const TargetEvidence send = Control("Send");
    const std::vector<DesktopOperation> routes = {
        DesktopOperation::PointerActivate,
        DesktopOperation::KeyActivate,
        DesktopOperation::Invoke,
        DesktopOperation::SetValue,
        DesktopOperation::TextEntryWithActivation};

    for (const DesktopOperation route : routes)
    {
        const auto decision = Decide(route, send, ConsequenceClass::Routine);
        Check(decision.verdict == AuthorizationVerdict::RequireApproval,
            "Sending was reachable without approval via " + ToString(route) + ".");
        Check(HasEffect(decision.effects, DesktopEffect::ExternalMessage),
            "The external-message effect was lost via " + ToString(route) + ".");
    }

    // And the same routes all open once the ceiling actually permits that effect.
    for (const DesktopOperation route : routes)
    {
        Check(Decide(route, send, ConsequenceClass::ExternalMessage).verdict ==
            AuthorizationVerdict::Allow,
            "A permitted effect was still refused via " + ToString(route) + ".");
    }
}

void TestMissingEvidenceIsNotTreatedAsHarmless()
{
    // The headline fix. Previously an unreadable target produced no dangerous keyword
    // and was therefore authorized, so the check passed most reliably exactly when it
    // knew least.
    TargetEvidence unreadable;
    unreadable.resolved = false;

    const auto autonomous = Decide(
        DesktopOperation::PointerActivate, unreadable, ConsequenceClass::Routine, true);
    Check(autonomous.evidence == EvidenceQuality::Missing,
        "An unresolved target was not reported as missing evidence.");
    Check(autonomous.verdict == AuthorizationVerdict::RequireApproval,
        "An unidentifiable target was clicked unprompted.");
    Check(autonomous.reason.find("Nothing identifiable") != std::string::npos,
        "The refusal did not say why: " + autonomous.reason);

    // Resolved but unnamed is the same situation: there is an element, and no
    // information about what it does.
    TargetEvidence unnamed = Control("");
    Check(Decide(DesktopOperation::Invoke, unnamed, ConsequenceClass::Routine).verdict ==
        AuthorizationVerdict::RequireApproval,
        "An unnamed control was invoked unprompted.");

    // An observation that has aged out describes a different moment.
    TargetEvidence stale = Control("Save");
    stale.stale = true;
    Check(Decide(DesktopOperation::PointerActivate, stale, ConsequenceClass::UserContent)
            .evidence == EvidenceQuality::Missing,
        "A stale observation was still treated as current evidence.");
}

void TestAmbiguousLabelsAreNotReadAsSafe()
{
    // "OK" names the gesture, not the consequence.
    for (const std::string label : {"OK", "Confirm", "Yes", "Continue", "Proceed"})
    {
        const auto decision =
            Decide(DesktopOperation::PointerActivate, Control(label), ConsequenceClass::Routine);
        Check(decision.evidence == EvidenceQuality::Ambiguous,
            "\"" + label + "\" was treated as a meaningful label.");
        Check(decision.verdict == AuthorizationVerdict::RequireApproval,
            "\"" + label + "\" was clicked unprompted.");
    }

    // Context supplies the meaning the label withholds. A Confirm inside a window about
    // deleting an account is not the same button as a Confirm inside Save preferences.
    const auto dangerous = Decide(
        DesktopOperation::PointerActivate,
        Control("Confirm", "Delete account - Settings"),
        ConsequenceClass::UserContent,
        /*autonomous=*/false);
    Check(HasEffect(dangerous.effects, DesktopEffect::AccountOrSecurity) &&
        dangerous.verdict == AuthorizationVerdict::RequireApproval,
        "A generic Confirm inside a dangerous dialog was authorized: " + dangerous.reason);

    const auto ordinary = Decide(
        DesktopOperation::PointerActivate,
        Control("Confirm", "Save preferences"),
        ConsequenceClass::UserContent,
        /*autonomous=*/false);
    Check(ordinary.verdict == AuthorizationVerdict::Allow,
        "An ordinary confirmation the user asked for was refused: " + ordinary.reason);
}

void TestEffectsCombineRatherThanRanking()
{
    // A ladder keeps only the larger of two effects. This control does both, and both
    // have to be permitted.
    const TargetEvidence shareAndSave = Control("Share", "Export and save report");
    const auto decision = Decide(
        DesktopOperation::PointerActivate, shareAndSave, ConsequenceClass::UserContent);
    Check(HasEffect(decision.effects, DesktopEffect::ExternalMessage) &&
        HasEffect(decision.effects, DesktopEffect::UserContent),
        "Two simultaneous effects were collapsed into one.");
    Check(decision.verdict == AuthorizationVerdict::RequireApproval,
        "A ceiling covering only the lesser effect authorized both.");

    Check(Decide(DesktopOperation::PointerActivate, shareAndSave,
            ConsequenceClass::ExternalMessage).verdict == AuthorizationVerdict::Allow,
        "A ceiling covering both effects still refused.");
}

void TestPasswordFieldsIdentifyThemselves()
{
    TargetEvidence secret;
    secret.resolved = true;
    secret.isPassword = true;
    // Deliberately unnamed: the flag is the evidence, not the label.
    const auto decision =
        Decide(DesktopOperation::TextEntry, secret, ConsequenceClass::Destructive);
    Check(decision.evidence == EvidenceQuality::Verified,
        "A password field was not treated as identified.");
    Check(HasEffect(decision.effects, DesktopEffect::AccountOrSecurity),
        "A password field did not carry a credential effect.");
    Check(decision.verdict == AuthorizationVerdict::RequireApproval,
        "Text was typed into a password field below the credential ceiling.");

    // Even a user-directed request stops here: the consequence is what is being gated.
    Check(Decide(DesktopOperation::TextEntry, secret, ConsequenceClass::Destructive,
            /*autonomous=*/false).verdict == AuthorizationVerdict::RequireApproval,
        "A user-directed request typed into a password field.");
}

void TestLookingIsNeverGated()
{
    TargetEvidence unreadable;
    unreadable.resolved = false;
    for (const DesktopOperation route :
         {DesktopOperation::Observe, DesktopOperation::KeyNavigate})
    {
        Check(Decide(route, unreadable, ConsequenceClass::Routine).verdict ==
            AuthorizationVerdict::Allow,
            "Non-committing " + ToString(route) + " was gated, which would make an "
            "unfamiliar window unlearnable.");
    }
    Check(!IsCommittingOperation(DesktopOperation::Observe) &&
        IsCommittingOperation(DesktopOperation::Invoke),
        "The committing/non-committing split is wrong.");
}

void TestUsefulAutonomyIsPreserved()
{
    TargetEvidence unreadable;
    unreadable.resolved = false;

    // A disposable workspace: an unknown label there is not a reason to stop, because
    // there is nothing in it that matters.
    Check(Decide(DesktopOperation::TextEntry, unreadable, ConsequenceClass::Routine,
            /*autonomous=*/true, /*scratch=*/true).verdict == AuthorizationVerdict::Allow,
        "Scratch work was blocked by an unreadable label.");

    // Someone asked for this specific thing, and their judgement is the evidence the
    // label failed to supply. Refusing here would make her useless when asked for help.
    Check(Decide(DesktopOperation::PointerActivate, unreadable, ConsequenceClass::Routine,
            /*autonomous=*/false).verdict == AuthorizationVerdict::Allow,
        "A user-directed click on an unlabelled control was refused.");

    // Ordinary named work inside the ceiling proceeds without ceremony.
    Check(Decide(DesktopOperation::TextEntry, Control("Document body"),
            ConsequenceClass::UserContent).verdict == AuthorizationVerdict::Allow,
        "Ordinary permitted editing required approval.");

    // But scratch is not a universal bypass: a recognized dangerous effect still stops.
    Check(Decide(DesktopOperation::PointerActivate, Control("Delete account"),
            ConsequenceClass::Routine, true, /*scratch=*/true).verdict ==
        AuthorizationVerdict::RequireApproval,
        "A scratch workspace authorized a known credential effect.");
}

void TestTheCeilingExpandsCompatibly()
{
    // Existing capability files store one ordered class. Each step must permit exactly
    // what the ordering already meant, so older files keep their current authority.
    Check(EffectsPermittedByCeiling(ConsequenceClass::Routine) == 0u,
        "The default ceiling permitted a special effect.");
    Check(HasEffect(EffectsPermittedByCeiling(ConsequenceClass::UserContent),
            DesktopEffect::UserContent) &&
        !HasEffect(EffectsPermittedByCeiling(ConsequenceClass::UserContent),
            DesktopEffect::ExternalMessage),
        "The user-content ceiling did not permit exactly its own step.");
    const DesktopEffects everything =
        EffectsPermittedByCeiling(ConsequenceClass::CommandSurface);
    Check(HasEffect(everything, DesktopEffect::CommandSurface) &&
        HasEffect(everything, DesktopEffect::UserContent),
        "The highest ceiling did not permit the steps below it.");
}

void TestRefusalsAreActionableAndCarryNoSecrets()
{
    TargetEvidence secret;
    secret.resolved = true;
    secret.isPassword = true;
    const auto decision =
        Decide(DesktopOperation::TextEntry, secret, ConsequenceClass::Routine);
    Check(!decision.reason.empty() &&
        decision.reason.find("credentials") != std::string::npos,
        "The refusal did not say what it was protecting: " + decision.reason);

    // The reason describes the effect and the control, never the text being typed --
    // an audit trail must not become the thing it was guarding.
    const auto named = Decide(
        DesktopOperation::PointerActivate, Control("Delete"), ConsequenceClass::Routine);
    Check(named.reason.find("Delete") != std::string::npos &&
        named.reason.find("destroys") != std::string::npos,
        "The refusal did not name the control and its effect: " + named.reason);
}

} // namespace

void RunDesktopAuthorizationTests()
{
    TestTheSameEffectCostsTheSameWhicheverRouteReachesIt();
    TestMissingEvidenceIsNotTreatedAsHarmless();
    TestAmbiguousLabelsAreNotReadAsSafe();
    TestEffectsCombineRatherThanRanking();
    TestPasswordFieldsIdentifyThemselves();
    TestLookingIsNeverGated();
    TestUsefulAutonomyIsPreserved();
    TestTheCeilingExpandsCompatibly();
    TestRefusalsAreActionableAndCarryNoSecrets();
    std::cout << "Desktop authorization tests passed: one rule, every route.\n";
}
