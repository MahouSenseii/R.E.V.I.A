#include "Computer/computerController.h"

#include <utility>

namespace revia::computer
{

namespace
{

// Whether a decision is one the runner can act on, as opposed to one that hands the
// question somewhere else.
//
// The distinction that the escalation rule turns on. WaitForState, Reobserve and
// NeedUser are answers -- bounded local control flow, or a person being asked -- and a
// provider that produced one has done its job. NeedReasoning and CannotHandle are the
// provider saying the question is not for it, which is the only case where paying for
// the expensive path is the right response.
bool IsAnswer(const ComputerDecisionKind kind)
{
    switch (kind)
    {
        case ComputerDecisionKind::ProposeAction:
        case ComputerDecisionKind::ProposeCompletion:
        case ComputerDecisionKind::NeedUser:
        case ComputerDecisionKind::WaitForState:
        case ComputerDecisionKind::Reobserve:
        case ComputerDecisionKind::NeedVision:
            return true;
        case ComputerDecisionKind::NeedReasoning:
        case ComputerDecisionKind::CannotHandle:
        default:
            return false;
    }
}

// Whether two decisions would have done the same thing.
//
// Compared on the action and what it aims at, never on the wording. Two providers that
// both propose pressing the same control agree, however differently they describe it,
// and a shadow comparison that counted wording would measure prose rather than
// behaviour.
bool SameIntent(const ComputerDecision& left, const ComputerDecision& right)
{
    if (left.kind != right.kind) return false;
    if (left.kind != ComputerDecisionKind::ProposeAction) return true;
    return left.step.action.type == right.step.action.type &&
        left.step.action.application == right.step.action.application &&
        left.step.action.control == right.step.action.control;
}

} // namespace

std::string ToString(const ComputerProviderMode value)
{
    switch (value)
    {
        case ComputerProviderMode::Shadow: return "shadow";
        case ComputerProviderMode::Assisted: return "assisted";
        case ComputerProviderMode::Learned: return "learned";
        case ComputerProviderMode::Legacy: break;
    }
    return "legacy";
}

ComputerProviderMode ComputerProviderModeFromString(const std::string& value)
{
    if (value == "shadow") return ComputerProviderMode::Shadow;
    if (value == "assisted") return ComputerProviderMode::Assisted;
    if (value == "learned") return ComputerProviderMode::Learned;
    // Anything unrecognised is Legacy. A configuration file with a typo in it must
    // leave behaviour where it was, not somewhere new.
    return ComputerProviderMode::Legacy;
}

ComputerController::ComputerController(const PayloadVault& payloadVault)
    : vault(&payloadVault)
{
}

void ComputerController::SetLegacyPolicy(std::unique_ptr<IComputerPolicy> policy)
{
    legacy = std::move(policy);
}

void ComputerController::SetRoutinePolicy(std::unique_ptr<RoutineComputerPolicy> policy)
{
    routine = std::move(policy);
}

void ComputerController::SetLearnedPolicy(std::unique_ptr<IComputerPolicy> policy)
{
    learned = std::move(policy);
}

void ComputerController::SetMode(const ComputerProviderMode mode)
{
    providerMode = mode;
}

bool ComputerController::SetSubgoal(ComputerSubgoal subgoal)
{
    // The seam where "validated" stops being a convention and becomes a check. A
    // subgoal that has not been through ValidateSubgoal has had no scope checked, no
    // origin attached and no payload confirmed, and handing one to a policy would make
    // every guarantee downstream of validation optional.
    if (!subgoal.Validated()) return false;
    if (!routine) return false;
    routine->SetSubgoal(std::move(subgoal));
    return true;
}

void ComputerController::ClearSubgoal()
{
    if (routine) routine->ClearSubgoal();
}

bool ComputerController::HasSubgoal() const
{
    return routine && routine->Subgoal().Validated();
}

ComputerSubgoal ComputerController::ActiveSubgoal() const
{
    return routine ? routine->Subgoal() : ComputerSubgoal{};
}

ComputerProviderMode ComputerController::EffectiveMode() const
{
    switch (providerMode)
    {
        case ComputerProviderMode::Learned:
            if (learned && learned->IsAvailable()) return ComputerProviderMode::Learned;
            // Falls back to Assisted rather than to Legacy: the routine policy is still
            // there and still cheaper, and a missing learned artifact is not a reason to
            // stop using the part that works.
            [[fallthrough]];
        case ComputerProviderMode::Assisted:
            if (routine && HasSubgoal()) return ComputerProviderMode::Assisted;
            return ComputerProviderMode::Legacy;
        case ComputerProviderMode::Shadow:
            return routine ? ComputerProviderMode::Shadow : ComputerProviderMode::Legacy;
        case ComputerProviderMode::Legacy:
        default:
            return ComputerProviderMode::Legacy;
    }
}

std::string ComputerController::ModeUnavailableReason() const
{
    const ComputerProviderMode effective = EffectiveMode();
    if (effective == providerMode) return {};

    if (providerMode == ComputerProviderMode::Learned &&
        (!learned || !learned->IsAvailable()))
    {
        return learned
            ? "No qualified learned artifact is loaded, so learned decisions are "
              "inactive and the routine policy is deciding instead."
            : "This build has no learned decision backend, so learned decisions are "
              "inactive.";
    }
    if (!routine)
    {
        return "No routine decision policy is installed.";
    }
    if (!HasSubgoal())
    {
        return "This task has no bounded subgoal, so there is nothing for a routine "
               "decision to work from.";
    }
    return "The selected decision mode is unavailable for this task.";
}

std::string ComputerController::ActiveProvider() const
{
    switch (EffectiveMode())
    {
        case ComputerProviderMode::Learned:
            return learned ? learned->Name() : std::string();
        case ComputerProviderMode::Assisted:
            return routine ? routine->Name() : std::string();
        case ComputerProviderMode::Shadow:
        case ComputerProviderMode::Legacy:
        default:
            return legacy ? legacy->Name() : std::string();
    }
}

void ComputerController::ResetStats()
{
    stats = ComputerControllerStats{};
    escalationsThisRun = 0;
}

ComputerController::Attempt ComputerController::Ask(
    IComputerPolicy& policy,
    const ComputerTaskContext& context,
    std::stop_token stopToken)
{
    Attempt attempt;
    if (!policy.IsAvailable())
    {
        attempt.decision.code = ComputerReasonCode::ProviderUnavailable;
        attempt.decision.provider = policy.Name();
        return attempt;
    }
    attempt.decision = policy.Decide(context, std::move(stopToken));
    if (attempt.decision.provider.empty()) attempt.decision.provider = policy.Name();
    attempt.usable = IsAnswer(attempt.decision.kind);
    return attempt;
}

bool ComputerController::Redeem(ComputerDecision& decision)
{
    if (!decision.payload.Valid()) return true;
    if (decision.kind != ComputerDecisionKind::ProposeAction) return true;

    if (vault == nullptr)
    {
        return false;
    }
    const std::optional<std::string> value = vault->Redeem(decision.payload);
    if (!value.has_value())
    {
        // Fails closed, and loudly. The alternative is a step that types nothing into a
        // field somebody is about to submit, which in a send-shaped workflow is an empty
        // message actually sent -- a wrong external effect produced by a missing lookup.
        return false;
    }
    decision.step.action.value = *value;
    return true;
}

void ComputerController::RunShadow(
    const ComputerTaskContext& context, std::stop_token stopToken)
{
    if (!routine) return;

    // The same context object the live provider was handed, which means the same
    // observation. Nothing here looks at the screen: a second look would bump the
    // observation generation and invalidate a visual target the live provider had
    // already chosen correctly, so the executor would refuse a click that was right
    // when it was decided.
    const Attempt shadow = Ask(*routine, context, std::move(stopToken));
    lastRecord.shadowEvaluated = true;
    lastRecord.shadowProvider = shadow.decision.provider;
    lastRecord.shadowKind = shadow.decision.kind;
    lastRecord.shadowDetail = shadow.decision.detail;
    ++stats.shadowComparisons;

    // Compared against what actually ran. The shadow's own step is deliberately not
    // kept: nothing downstream should be able to reach a proposal that was never
    // authorized, and the surest way to guarantee that is not to carry it.
    ComputerDecision live;
    live.kind = lastRecord.kind;
    live.step.action.type = liveAction.type;
    live.step.action.application = liveAction.application;
    live.step.action.control = liveAction.control;
    lastRecord.shadowAgreed = SameIntent(live, shadow.decision);
    if (lastRecord.shadowAgreed) ++stats.shadowAgreements;
}

goals::NextStep ComputerController::Decide(
    const ComputerTaskContext& context, std::stop_token stopToken)
{
    lastRecord = ComputerDecisionRecord{};
    ++stats.decisions;
    goals::NextStep next;

    if (!legacy)
    {
        // The baseline is the one provider that must exist. Without it there is no
        // fallback, and a coordinator with nothing to coordinate says so rather than
        // letting a cheaper provider become the only answer by default.
        lastRecord.code = ComputerReasonCode::ProviderUnavailable;
        lastRecord.detail = "The next step could not be decided.";
        next.reason = lastRecord.detail;
        return next;
    }

    const ComputerProviderMode effective = EffectiveMode();
    ComputerDecision chosen;
    bool escalated = false;
    std::string escalatedFrom;

    // The cheap providers first, in the order the mode asks for. Each is asked once per
    // iteration, which is what keeps two providers from handing a decision back and
    // forth inside one step.
    const bool preferCheap =
        (effective == ComputerProviderMode::Assisted ||
         effective == ComputerProviderMode::Learned) &&
        escalationsThisRun < escalationBudget;

    if (preferCheap)
    {
        if (effective == ComputerProviderMode::Learned && learned)
        {
            Attempt attempt = Ask(*learned, context, stopToken);
            if (attempt.usable && Redeem(attempt.decision))
            {
                chosen = std::move(attempt.decision);
                ++stats.learnedDecisions;
            }
            else
            {
                if (attempt.usable) ++stats.payloadRefusals;
                escalated = true;
                escalatedFrom = attempt.decision.provider;
            }
        }

        if (chosen.provider.empty() && routine)
        {
            Attempt attempt = Ask(*routine, context, stopToken);
            if (attempt.usable && Redeem(attempt.decision))
            {
                chosen = std::move(attempt.decision);
                ++stats.routineDecisions;
                escalated = false;
                escalatedFrom.clear();
            }
            else
            {
                if (attempt.usable) ++stats.payloadRefusals;
                escalated = true;
                escalatedFrom = attempt.decision.provider;
            }
        }
    }

    if (chosen.provider.empty())
    {
        // The existing path, unchanged. Everything above this line is an attempt to
        // avoid reaching it; nothing above this line is allowed to change what happens
        // when it is reached.
        if (escalated)
        {
            ++escalationsThisRun;
            ++stats.escalations;
        }
        Attempt attempt = Ask(*legacy, context, stopToken);
        if (!Redeem(attempt.decision))
        {
            ++stats.payloadRefusals;
            attempt.decision.kind = ComputerDecisionKind::CannotHandle;
            attempt.decision.code = ComputerReasonCode::ProviderFailed;
            attempt.decision.detail =
                "The content this step would have entered is no longer available.";
        }
        chosen = std::move(attempt.decision);
        ++stats.legacyDecisions;
        lastRecord.reachedModel = true;
        ++stats.modelCalls;
    }

    lastRecord.provider = chosen.provider;
    lastRecord.kind = chosen.kind;
    lastRecord.code = chosen.code;
    lastRecord.detail = chosen.detail;
    lastRecord.tokens = chosen.tokens;
    lastRecord.costReported = chosen.costReported;
    lastRecord.escalated = escalated;
    lastRecord.escalatedFrom = escalatedFrom;
    stats.tokens += chosen.tokens;
    liveAction = chosen.step.action;
    lastRecord.action = chosen.kind == ComputerDecisionKind::ProposeAction
        ? chosen.step.action.type : actions::ActionType::Unknown;
    // Resolved back to the name the observation reported, so the record carries what a
    // person would read off the screen rather than this window's automation id.
    if (lastRecord.action != actions::ActionType::Unknown)
    {
        lastRecord.targetId = chosen.step.action.control;
        lastRecord.targetName.clear();
        for (const ObservedCandidate& candidate : context.observation.candidates)
        {
            if (candidate.id == chosen.step.action.control)
            {
                // Empty when the control publishes no name. The id above is what joins
                // this decision to its candidate; the name is what a person reads.
                lastRecord.targetName = candidate.name;
                break;
            }
        }
    }

    next.tokens = chosen.tokens;
    next.costReported = chosen.costReported;

    switch (chosen.kind)
    {
        case ComputerDecisionKind::ProposeAction:
            next.hasStep = true;
            next.step = chosen.step;
            break;

        case ComputerDecisionKind::ProposeCompletion:
            // A proposal, not a verdict. The runner decides whether the evidence
            // actually supports finishing.
            next.finished = true;
            next.reason = chosen.detail;
            break;

        // Everything below is a real answer that is neither a step nor a completion.
        // The runner records it as Undecided, which is honest: nothing went wrong and
        // nothing finished.
        case ComputerDecisionKind::NeedReasoning:
        case ComputerDecisionKind::NeedVision:
        case ComputerDecisionKind::NeedUser:
        case ComputerDecisionKind::WaitForState:
        case ComputerDecisionKind::Reobserve:
        case ComputerDecisionKind::CannotHandle:
        default:
            next.reason = chosen.detail.empty()
                ? "No next action was proposed." : chosen.detail;
            break;
    }

    // Last, and after the live answer is fixed. A comparison that ran first could
    // influence what ran, which would make it a second decision rather than a
    // measurement of one.
    if (effective == ComputerProviderMode::Shadow)
    {
        RunShadow(context, std::move(stopToken));
    }

    return next;
}

} // namespace revia::computer
