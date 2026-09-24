#include "Computer/computerTypes.h"
#include "Computer/computerSubgoal.h"

#include <atomic>
#include <chrono>
#include <sstream>

namespace revia::computer
{

std::string ToString(const ComputerDecisionKind value)
{
    switch (value)
    {
        case ComputerDecisionKind::ProposeAction: return "propose_action";
        case ComputerDecisionKind::NeedReasoning: return "need_reasoning";
        case ComputerDecisionKind::NeedVision: return "need_vision";
        case ComputerDecisionKind::NeedUser: return "need_user";
        case ComputerDecisionKind::WaitForState: return "wait_for_state";
        case ComputerDecisionKind::Reobserve: return "reobserve";
        case ComputerDecisionKind::ProposeCompletion: return "propose_completion";
        case ComputerDecisionKind::CannotHandle: return "cannot_handle";
    }
    return "cannot_handle";
}

std::string ToString(const ComputerReasonCode value)
{
    switch (value)
    {
        case ComputerReasonCode::None: return "none";
        case ComputerReasonCode::ProviderUnavailable: return "provider_unavailable";
        case ComputerReasonCode::ProviderFailed: return "provider_failed";
        case ComputerReasonCode::MalformedProviderOutput: return "malformed_provider_output";
        case ComputerReasonCode::NoActionProposed: return "no_action_proposed";
        case ComputerReasonCode::ObservationUnavailable: return "observation_unavailable";
        case ComputerReasonCode::OutsideQualifiedScope: return "outside_qualified_scope";
    }
    return "none";
}

std::string ToString(const SubgoalIntent value)
{
    switch (value)
    {
        case SubgoalIntent::LaunchApplication: return "launch_application";
        case SubgoalIntent::FocusWindow: return "focus_window";
        case SubgoalIntent::ResolveTarget: return "resolve_target";
        case SubgoalIntent::InteractWithControl: return "interact_with_control";
        case SubgoalIntent::EnterPayload: return "enter_payload";
        case SubgoalIntent::WaitForState: return "wait_for_state";
        case SubgoalIntent::Escalate: return "escalate";
        case SubgoalIntent::Unspecified: break;
    }
    return "unspecified";
}

SubgoalIntent SubgoalIntentFromString(const std::string& value)
{
    if (value == "launch_application") return SubgoalIntent::LaunchApplication;
    if (value == "focus_window") return SubgoalIntent::FocusWindow;
    if (value == "resolve_target") return SubgoalIntent::ResolveTarget;
    if (value == "interact_with_control") return SubgoalIntent::InteractWithControl;
    if (value == "enter_payload") return SubgoalIntent::EnterPayload;
    if (value == "wait_for_state") return SubgoalIntent::WaitForState;
    if (value == "escalate") return SubgoalIntent::Escalate;
    // Anything unrecognised is Unspecified rather than a nearest guess. A name this
    // build does not know is a contract this build does not implement, and validation
    // refuses it by that name.
    return SubgoalIntent::Unspecified;
}

std::string ToString(const RequestOrigin value)
{
    switch (value)
    {
        case RequestOrigin::UserDirected: return "user_directed";
        case RequestOrigin::Autonomous: return "autonomous";
        case RequestOrigin::Unknown: break;
    }
    return "unknown";
}

std::string ToString(const SubgoalRejection value)
{
    switch (value)
    {
        case SubgoalRejection::UnsupportedSchema: return "unsupported_schema";
        case SubgoalRejection::UnsupportedIntent: return "unsupported_intent";
        case SubgoalRejection::IncompleteForIntent: return "incomplete_for_intent";
        case SubgoalRejection::OutsideScope: return "outside_scope";
        case SubgoalRejection::UnknownPayload: return "unknown_payload";
        case SubgoalRejection::UnverifiablePostcondition:
            return "unverifiable_postcondition";
        case SubgoalRejection::OversizedField: return "oversized_field";
        case SubgoalRejection::SendBeforePlacement: return "send_before_placement";
        case SubgoalRejection::None: break;
    }
    return "none";
}

std::string NewSubgoalId()
{
    static std::atomic<std::uint64_t> counter{1};
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream stream;
    stream << "subgoal-" << ticks << '-'
           << counter.fetch_add(1, std::memory_order_relaxed);
    return stream.str();
}

// The standard UI Automation control type ids, as names.
//
// Only the ones a bounded desktop decision actually distinguishes. The table is
// deliberately partial: an unlisted id returns an empty role, and an empty role is
// matched only by a descriptor that left the role unconstrained. A wrong guess here
// would be a descriptor silently matching the wrong kind of thing, which is worse than
// a descriptor that has to be more specific.
std::string ControlRoleName(const int controlType)
{
    switch (controlType)
    {
        case 50000: return "button";
        case 50002: return "checkbox";
        case 50003: return "combobox";
        case 50004: return "edit";
        case 50005: return "hyperlink";
        case 50007: return "listitem";
        case 50008: return "list";
        case 50011: return "menuitem";
        case 50013: return "radiobutton";
        case 50019: return "tabitem";
        case 50020: return "text";
        case 50024: return "treeitem";
        case 50026: return "group";
        case 50029: return "dataitem";
        case 50030: return "document";
        case 50031: return "splitbutton";
        case 50032: return "window";
        case 50033: return "pane";
        default: break;
    }
    return {};
}

} // namespace revia::computer
