#include "Policy/desktopAuthorization.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace revia::policy
{

namespace
{

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// Word-bounded, because substring matching turns "Display settings" into a payment.
bool ContainsPhrase(const std::string& haystack, const std::string& phrase)
{
    const auto boundary = [](const char character)
    {
        return !std::isalnum(static_cast<unsigned char>(character));
    };
    std::size_t at = haystack.find(phrase);
    while (at != std::string::npos)
    {
        const bool startsClean = at == 0 || boundary(haystack[at - 1]);
        const std::size_t after = at + phrase.size();
        const bool endsClean = after >= haystack.size() || boundary(haystack[after]);
        if (startsClean && endsClean)
        {
            return true;
        }
        at = haystack.find(phrase, at + 1);
    }
    return false;
}

bool MatchesAny(const std::string& value, const std::vector<std::string>& phrases)
{
    return std::any_of(phrases.begin(), phrases.end(),
        [&value](const std::string& phrase) { return ContainsPhrase(value, phrase); });
}

// Labels that name the gesture instead of the consequence. "OK" is not information
// about what is being agreed to, and treating it as though it were is how a dialog box
// gets clicked because it looked friendly.
bool IsAmbiguousAffirmative(const std::string& lowered)
{
    static const std::vector<std::string> affirmatives = {
        "ok", "okay", "yes", "confirm", "continue", "proceed", "accept", "agree",
        "next", "finish", "done", "got it", "understood", "sure"};
    return MatchesAny(lowered, affirmatives);
}

} // namespace

bool IsCommittingOperation(const DesktopOperation operation)
{
    switch (operation)
    {
        case DesktopOperation::Observe:
        case DesktopOperation::KeyNavigate:
            return false;
        default:
            return true;
    }
}

std::string ToString(const DesktopOperation operation)
{
    switch (operation)
    {
        case DesktopOperation::Observe: return "observe";
        case DesktopOperation::PointerActivate: return "pointer_activate";
        case DesktopOperation::KeyActivate: return "key_activate";
        case DesktopOperation::KeyNavigate: return "key_navigate";
        case DesktopOperation::TextEntry: return "text_entry";
        case DesktopOperation::TextEntryWithActivation: return "text_entry_with_activation";
        case DesktopOperation::SetValue: return "set_value";
        case DesktopOperation::Invoke: return "invoke";
        case DesktopOperation::LaunchApplication: return "launch_application";
    }
    return "observe";
}

std::string ToString(const EvidenceQuality quality)
{
    switch (quality)
    {
        case EvidenceQuality::Verified: return "verified";
        case EvidenceQuality::Ambiguous: return "ambiguous";
        case EvidenceQuality::Missing: return "missing";
    }
    return "missing";
}

std::string ToString(const AuthorizationVerdict verdict)
{
    switch (verdict)
    {
        case AuthorizationVerdict::Allow: return "allow";
        case AuthorizationVerdict::RequireApproval: return "require_approval";
        case AuthorizationVerdict::Refuse: return "refuse";
    }
    return "refuse";
}

std::string DescribeEffects(const DesktopEffects effects)
{
    if (effects == 0u)
    {
        return "nothing beyond ordinary interaction";
    }
    std::string described;
    const auto add = [&described](const char* name)
    {
        if (!described.empty()) described += " + ";
        described += name;
    };
    if (HasEffect(effects, DesktopEffect::UserContent)) add("changes your content");
    if (HasEffect(effects, DesktopEffect::ExternalMessage)) add("sends something out");
    if (HasEffect(effects, DesktopEffect::Financial)) add("spends money");
    if (HasEffect(effects, DesktopEffect::Destructive)) add("destroys something");
    if (HasEffect(effects, DesktopEffect::AccountOrSecurity)) add("touches credentials or access");
    if (HasEffect(effects, DesktopEffect::CommandSurface)) add("runs commands");
    return described;
}

DesktopEffects EffectsPermittedByCeiling(const actions::ConsequenceClass ceiling)
{
    // The stored ceiling is one ordered class so existing capability files keep working.
    // Each step permits everything below it, which is what the ordering already meant.
    DesktopEffects permitted = 0u;
    const int level = static_cast<int>(ceiling);
    if (level >= static_cast<int>(actions::ConsequenceClass::UserContent))
        permitted = permitted | DesktopEffect::UserContent;
    if (level >= static_cast<int>(actions::ConsequenceClass::ExternalMessage))
        permitted = permitted | DesktopEffect::ExternalMessage;
    if (level >= static_cast<int>(actions::ConsequenceClass::Financial))
        permitted = permitted | DesktopEffect::Financial;
    if (level >= static_cast<int>(actions::ConsequenceClass::Destructive))
        permitted = permitted | DesktopEffect::Destructive;
    if (level >= static_cast<int>(actions::ConsequenceClass::AccountOrSecurity))
        permitted = permitted | DesktopEffect::AccountOrSecurity;
    if (level >= static_cast<int>(actions::ConsequenceClass::CommandSurface))
        permitted = permitted | DesktopEffect::CommandSurface;
    return permitted;
}

EvidenceQuality AssessEvidence(const AuthorizationRequest& request)
{
    const TargetEvidence& evidence = request.evidence;
    // A password field identifies itself, so it is known even when nothing else is.
    if (evidence.isPassword)
    {
        return EvidenceQuality::Verified;
    }
    // An observation that has aged out describes a machine that may have moved on. It is
    // not weaker evidence, it is evidence about a different moment.
    if (evidence.stale || !evidence.resolved || evidence.controlName.empty())
    {
        return EvidenceQuality::Missing;
    }
    if (IsAmbiguousAffirmative(Lower(evidence.controlName)))
    {
        return EvidenceQuality::Ambiguous;
    }
    return EvidenceQuality::Verified;
}

DesktopEffects AssessEffects(const AuthorizationRequest& request)
{
    DesktopEffects effects = 0u;
    const TargetEvidence& evidence = request.evidence;

    // The operation contributes what it does regardless of the label. Putting a value
    // into a field changes content whether or not the field is named something alarming.
    if (request.operation == DesktopOperation::SetValue ||
        request.operation == DesktopOperation::TextEntry ||
        request.operation == DesktopOperation::TextEntryWithActivation)
    {
        effects = effects | DesktopEffect::UserContent;
    }

    if (evidence.isPassword)
    {
        return effects | DesktopEffect::AccountOrSecurity;
    }

    // The label and its surrounding context, matched together. A "Confirm" inside a
    // window titled "Delete account" carries the window's meaning, which is the whole
    // reason context is read at all.
    const std::string label = Lower(evidence.controlName);
    const std::string context = Lower(evidence.windowTitle);

    static const std::vector<std::string> commandSurface = {
        "command prompt", "powershell", "terminal", "run command", "execute command",
        "developer console"};
    static const std::vector<std::string> accountOrSecurity = {
        "password", "passphrase", "sign in", "signin", "log in", "login", "sign out",
        "log out", "credential", "api key", "access token", "two-factor", "2fa",
        "permission", "permissions", "administrator", "delete account",
        "deactivate account", "close account", "change email", "security", "privacy",
        "grant access", "authorize", "private key"};
    static const std::vector<std::string> financial = {
        "buy", "buy now", "purchase", "pay", "payment", "checkout", "check out",
        "place order", "confirm order", "subscribe", "donate", "send money", "transfer"};
    static const std::vector<std::string> destructive = {
        "delete", "delete all", "remove", "erase", "format", "wipe", "empty trash",
        "empty recycle bin", "permanently", "destroy", "uninstall"};
    static const std::vector<std::string> externalMessage = {
        "send", "post", "publish", "share", "tweet", "upload", "submit", "reply",
        "broadcast", "go live", "invite", "email"};
    static const std::vector<std::string> userContent = {
        "save", "save as", "rename", "move", "overwrite", "replace", "apply", "commit",
        "export", "import", "merge"};

    const auto either = [&](const std::vector<std::string>& phrases)
    {
        return MatchesAny(label, phrases) || MatchesAny(context, phrases);
    };

    // Additive on purpose: a control can be more than one of these at once, and taking
    // only the highest would drop the rest.
    if (either(commandSurface)) effects = effects | DesktopEffect::CommandSurface;
    if (either(accountOrSecurity)) effects = effects | DesktopEffect::AccountOrSecurity;
    if (either(destructive)) effects = effects | DesktopEffect::Destructive;
    if (either(financial)) effects = effects | DesktopEffect::Financial;
    if (either(externalMessage)) effects = effects | DesktopEffect::ExternalMessage;
    if (either(userContent)) effects = effects | DesktopEffect::UserContent;
    return effects;
}

AuthorizationDecision AuthorizeDesktopEffect(
    const AuthorizationRequest& request,
    const actions::CapabilitySettings::DesktopControl& settings)
{
    AuthorizationDecision decision;
    decision.effects = AssessEffects(request);
    decision.evidence = AssessEvidence(request);

    // Looking is not committing. Refusing to inspect an unfamiliar window because its
    // controls are unfamiliar would make the machine unlearnable.
    if (!IsCommittingOperation(request.operation))
    {
        decision.verdict = AuthorizationVerdict::Allow;
        decision.reason = "Observation and navigation commit nothing.";
        return decision;
    }

    DesktopEffects permitted = EffectsPermittedByCeiling(
        settings.maxUnconfirmedConsequence);
    if (request.insideApprovedScratch)
    {
        // Content in a disposable workspace is not the user's content, so editing it
        // freely is the whole point of having one. This belongs in the permitted set
        // rather than as a later override: scratch widens what is ordinary, it does not
        // excuse an effect that reaches past the workspace. Sending, spending, deleting
        // and touching credentials are all still weighed below.
        permitted = permitted | DesktopEffect::UserContent;
    }
    const DesktopEffects unpermitted = decision.effects & ~permitted;

    if (unpermitted != 0u)
    {
        // Known and above the line. This one does not depend on who asked: a person
        // asking for a click on "Delete account" still gets stopped, because the point
        // of the boundary is the consequence rather than the requester's confidence.
        decision.verdict = AuthorizationVerdict::RequireApproval;
        decision.reason = "\"" +
            (request.evidence.controlName.empty() ? std::string("an unnamed control")
                                                  : request.evidence.controlName) +
            "\" " + DescribeEffects(unpermitted) +
            ", which is above what is currently allowed without asking.";
        return decision;
    }

    if (decision.evidence == EvidenceQuality::Verified)
    {
        decision.verdict = AuthorizationVerdict::Allow;
        decision.reason = "The target is identified and its effect is within scope.";
        return decision;
    }

    // From here the evidence is Missing or Ambiguous, and the honest position is that
    // the effect is unknown rather than absent. What happens next depends on whether
    // anyone chose this target.
    if (request.insideApprovedScratch)
    {
        decision.verdict = AuthorizationVerdict::Allow;
        decision.reason =
            "The target is unclear, but this is a disposable workspace where that is "
            "not a reason to stop.";
        return decision;
    }
    if (!request.autonomousOrigin)
    {
        // Someone asked for this specific thing. Their judgement is the evidence the
        // label failed to supply, and refusing here would make her useless at exactly
        // the moment she was asked for help.
        decision.verdict = AuthorizationVerdict::Allow;
        decision.reason = decision.evidence == EvidenceQuality::Ambiguous
            ? "The label does not say what it does; proceeding because you asked for it."
            : "The target could not be identified; proceeding because you asked for it.";
        return decision;
    }

    decision.verdict = AuthorizationVerdict::RequireApproval;
    decision.reason = decision.evidence == EvidenceQuality::Ambiguous
        ? "\"" + request.evidence.controlName +
            "\" does not say what it does, and nobody asked for this specific step."
        : "Nothing identifiable is at that target, and nobody asked for this specific step.";
    return decision;
}

std::string PolicyVersion(const actions::CapabilitySettings::DesktopControl& settings)
{
    // Every switch that can change an authorization outcome, in a fixed order. A plain
    // readable digest rather than a hash: when a binding is refused for a policy change,
    // the record should say which policy it was made under.
    std::string version = "dc1:";
    version += settings.pointer ? 'p' : '-';
    version += settings.keyboard ? 'k' : '-';
    version += settings.applicationLaunch ? 'l' : '-';
    version += settings.rawCoordinates ? 'r' : '-';
    version += settings.allowCommandSurfaces ? 'c' : '-';
    version += settings.autonomous ? 'a' : '-';
    version += settings.scope ==
        actions::CapabilitySettings::DesktopControl::InputScope::WholeDesktop ? 'w' : '-';
    version += ':';
    version += actions::ToString(settings.maxUnconfirmedConsequence);
    return version;
}

bool AuthorizeOrExplain(
    const DesktopOperation operation,
    const TargetEvidence& evidence,
    const actions::CapabilitySettings::DesktopControl& settings,
    const actions::ActionRequest& request,
    std::string& outFailure)
{
    AuthorizationRequest ask;
    ask.operation = operation;
    ask.evidence = evidence;
    // Runtime-owned. The session stamps requestedBy at its entry points and no field in
    // model-authored JSON reaches it, so this cannot be talked into saying "user".
    ask.autonomousOrigin = actions::IsAutonomousRequest(request.requestedBy);
    const AuthorizationDecision decision = AuthorizeDesktopEffect(ask, settings);
    if (decision.verdict == AuthorizationVerdict::Allow)
    {
        outFailure.clear();
        return true;
    }
    outFailure = "Refused: " + decision.reason;
    return false;
}

} // namespace revia::policy
