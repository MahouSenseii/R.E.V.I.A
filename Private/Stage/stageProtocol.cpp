#include "Stage/stageProtocol.h"

#include "Core/utf8.h"

#include <nlohmann/json.hpp>

namespace revia::stage
{

namespace
{
using nlohmann::json;

std::string Text(const json& value, const char* key)
{
    if (!value.is_object() || !value.contains(key)) return {};
    const json& field = value[key];
    return field.is_string() ? field.get<std::string>() : std::string();
}

int Number(const json& value, const char* key, const int fallback = 0)
{
    if (!value.is_object() || !value.contains(key) || !value[key].is_number_integer()) return fallback;
    return value[key].get<int>();
}

bool Flag(const json& value, const char* key, const bool fallback = false)
{
    if (!value.is_object() || !value.contains(key) || !value[key].is_boolean()) return fallback;
    return value[key].get<bool>();
}

bool Parse(const std::string& line, json& out, std::string& outError)
{
    try
    {
        out = json::parse(line);
    }
    catch (const std::exception& error)
    {
        outError = std::string("not JSON: ") + error.what();
        return false;
    }
    if (!out.is_object())
    {
        outError = "not an object";
        return false;
    }
    return true;
}
} // namespace

const char* ToString(const StageTier tier)
{
    switch (tier)
    {
        case StageTier::Observe: return "T0 observe";
        case StageTier::Confined: return "T1 confined";
        case StageTier::Desktop: return "T2 desktop";
        case StageTier::System: return "T3 system";
    }
    return "unknown";
}

bool TierFromInt(const int value, StageTier& outTier)
{
    if (value < 0 || value > 3) return false;
    outTier = static_cast<StageTier>(value);
    return true;
}

StageTier TierFor(const actions::ActionRequest& request)
{
    using actions::ActionType;
    switch (request.type)
    {
        case ActionType::InspectWindow:
            return StageTier::Observe;
        case ActionType::LaunchApplication:
            return StageTier::System;
        case ActionType::FocusWindow:
        case ActionType::SetControlText:
        case ActionType::InvokeControl:
            return StageTier::Confined;
        case ActionType::MoveCursor:
        case ActionType::ClickPointer:
        case ActionType::DragPointer:
        case ActionType::ScrollPointer:
        case ActionType::PressKeys:
        case ActionType::TypeText:
            // Input that names its application stays inside it; input aimed at the
            // screen is the whole desktop's.
            return request.application.empty() ? StageTier::Desktop : StageTier::Confined;
        default:
            return StageTier::System;
    }
}

std::string EncodeHello(const GuestInfo& guest)
{
    return json({
        {"type", "hello"},
        {"name", guest.name},
        {"version", guest.version},
        {"tier", static_cast<int>(guest.grantedTier)},
        {"checkpoint", guest.checkpoint}}).dump();
}

std::string EncodeRequest(const std::string& id, const actions::ActionRequest& request, const StageTier tier)
{
    const auto& input = request.input;
    const auto& resolution = request.resolution;
    return json({
        {"type", "request"},
        {"id", id},
        {"tier", static_cast<int>(tier)},
        {"action", {
            {"id", request.id},
            {"type", actions::ToString(request.type)},
            {"source", actions::PathToUtf8(request.source)},
            {"destination", actions::PathToUtf8(request.destination)},
            {"application", request.application},
            {"window_title", request.windowTitle},
            {"control", request.control},
            {"value", request.value},
            {"arguments", request.arguments},
            {"requested_by", request.requestedBy},
            {"dry_run", request.dryRun},
            {"input", {
                {"x", input.x}, {"y", input.y}, {"has_point", input.hasPoint},
                {"end_x", input.endX}, {"end_y", input.endY}, {"has_end_point", input.hasEndPoint},
                {"button", static_cast<int>(input.button)}, {"click_count", input.clickCount},
                {"scroll_clicks", input.scrollClicks}, {"horizontal_scroll", input.horizontalScroll},
                {"keys", input.keys},
                {"end_region", {input.endRegionLeft, input.endRegionTop, input.endRegionRight, input.endRegionBottom}}}},
            {"resolution", {
                {"kind", static_cast<int>(resolution.kind)},
                {"model_target", resolution.modelTarget},
                {"region", {resolution.regionLeft, resolution.regionTop, resolution.regionRight, resolution.regionBottom}},
                {"resolved_name", resolution.resolvedName},
                {"resolved_automation_id", resolution.resolvedAutomationId},
                {"resolved_runtime_id", resolution.resolvedRuntimeId},
                {"resolved_control_type", resolution.resolvedControlType}}}}}}).dump();
}

std::string EncodeResult(const std::string& id, const actions::ActionResult& result)
{
    json entries = json::array();
    for (const std::string& entry : result.entries)
    {
        if (entries.size() >= MostResultEntries) break;
        entries.push_back(utf8::Prefix(entry, 2000));
    }
    return json({
        {"type", "result"},
        {"id", id},
        {"attempted", result.attempted},
        {"succeeded", result.succeeded},
        {"dry_run", result.dryRun},
        {"message", utf8::Prefix(result.message, 4000)},
        {"content", utf8::Prefix(result.content, LongestResultContent)},
        {"entries", entries},
        {"backend", result.backend}}).dump();
}

std::string EncodeRefusal(const std::string& id, const std::string& reason)
{
    return json({{"type", "refused"}, {"id", id}, {"reason", utf8::Prefix(reason, 2000)}}).dump();
}

std::string EncodeHalt()
{
    return json({{"type", "halt"}}).dump();
}

std::string EncodePing()
{
    return json({{"type", "ping"}}).dump();
}

std::string EncodePong()
{
    return json({{"type", "pong"}}).dump();
}

bool Decode(const std::string& line, Envelope& outEnvelope, std::string& outError)
{
    json message;
    if (!Parse(line, message, outError)) return false;
    outEnvelope.type = Text(message, "type");
    outEnvelope.id = Text(message, "id");
    if (outEnvelope.type.empty())
    {
        outError = "no type";
        return false;
    }
    return true;
}

bool ReadHello(const std::string& line, GuestInfo& outGuest, std::string& outError)
{
    json message;
    if (!Parse(line, message, outError)) return false;
    if (Text(message, "type") != "hello")
    {
        outError = "the guest did not say hello first";
        return false;
    }
    outGuest.name = utf8::Prefix(Text(message, "name"), 80);
    outGuest.version = utf8::Prefix(Text(message, "version"), 40);
    outGuest.checkpoint = utf8::Prefix(Text(message, "checkpoint"), 120);
    if (!TierFromInt(Number(message, "tier", -1), outGuest.grantedTier))
    {
        outError = "the guest named no tier it was started with";
        return false;
    }
    if (outGuest.name.empty()) outGuest.name = "stage guest";
    return true;
}

bool ReadRequest(const std::string& line, actions::ActionRequest& outRequest, StageTier& outTier, std::string& outError)
{
    json message;
    if (!Parse(line, message, outError)) return false;
    if (Text(message, "type") != "request" || !message.contains("action") || !message["action"].is_object())
    {
        outError = "not a request";
        return false;
    }
    if (!TierFromInt(Number(message, "tier", -1), outTier))
    {
        outError = "the request names no tier";
        return false;
    }
    const json& action = message["action"];
    actions::ActionRequest request;
    request.id = Text(action, "id");
    request.type = actions::ActionTypeFromString(Text(action, "type"));
    if (request.type == actions::ActionType::Unknown)
    {
        outError = "unknown action type " + Text(action, "type");
        return false;
    }
    request.source = actions::Utf8ToPath(Text(action, "source"));
    request.destination = actions::Utf8ToPath(Text(action, "destination"));
    request.application = Text(action, "application");
    request.windowTitle = Text(action, "window_title");
    request.control = Text(action, "control");
    request.value = Text(action, "value");
    request.arguments = Text(action, "arguments");
    request.requestedBy = Text(action, "requested_by");
    request.dryRun = Flag(action, "dry_run");
    if (action.contains("input") && action["input"].is_object())
    {
        const json& input = action["input"];
        request.input.x = Number(input, "x");
        request.input.y = Number(input, "y");
        request.input.hasPoint = Flag(input, "has_point");
        request.input.endX = Number(input, "end_x");
        request.input.endY = Number(input, "end_y");
        request.input.hasEndPoint = Flag(input, "has_end_point");
        const int button = Number(input, "button");
        request.input.button = button >= 0 && button <= 2
            ? static_cast<actions::ActionRequest::DesktopInput::PointerButton>(button)
            : actions::ActionRequest::DesktopInput::PointerButton::Left;
        request.input.clickCount = Number(input, "click_count", 1);
        request.input.scrollClicks = Number(input, "scroll_clicks");
        request.input.horizontalScroll = Flag(input, "horizontal_scroll");
        request.input.keys = Text(input, "keys");
        if (input.contains("end_region") && input["end_region"].is_array() && input["end_region"].size() == 4)
        {
            const json& region = input["end_region"];
            if (region[0].is_number_integer() && region[1].is_number_integer() &&
                region[2].is_number_integer() && region[3].is_number_integer())
            {
                request.input.endRegionLeft = region[0].get<int>();
                request.input.endRegionTop = region[1].get<int>();
                request.input.endRegionRight = region[2].get<int>();
                request.input.endRegionBottom = region[3].get<int>();
            }
        }
    }
    if (action.contains("resolution") && action["resolution"].is_object())
    {
        const json& resolution = action["resolution"];
        const int kind = Number(resolution, "kind");
        request.resolution.kind = kind >= 0 && kind <= 3
            ? static_cast<actions::TargetResolutionKind>(kind) : actions::TargetResolutionKind::None;
        request.resolution.modelTarget = Text(resolution, "model_target");
        request.resolution.resolvedName = Text(resolution, "resolved_name");
        request.resolution.resolvedAutomationId = Text(resolution, "resolved_automation_id");
        request.resolution.resolvedRuntimeId = Text(resolution, "resolved_runtime_id");
        request.resolution.resolvedControlType = Number(resolution, "resolved_control_type");
        if (resolution.contains("region") && resolution["region"].is_array() && resolution["region"].size() == 4)
        {
            const json& region = resolution["region"];
            if (region[0].is_number_integer() && region[1].is_number_integer() &&
                region[2].is_number_integer() && region[3].is_number_integer())
            {
                request.resolution.regionLeft = region[0].get<int>();
                request.resolution.regionTop = region[1].get<int>();
                request.resolution.regionRight = region[2].get<int>();
                request.resolution.regionBottom = region[3].get<int>();
            }
        }
    }
    outRequest = std::move(request);
    return true;
}

bool ReadResult(const std::string& line, actions::ActionResult& outResult, std::string& outError)
{
    json message;
    if (!Parse(line, message, outError)) return false;
    if (Text(message, "type") != "result")
    {
        outError = "not a result";
        return false;
    }
    actions::ActionResult result;
    result.attempted = Flag(message, "attempted");
    result.succeeded = Flag(message, "succeeded");
    result.dryRun = Flag(message, "dry_run");
    result.message = utf8::Prefix(Text(message, "message"), 4000);
    result.content = utf8::Prefix(Text(message, "content"), LongestResultContent);
    result.backend = Text(message, "backend");
    if (message.contains("entries") && message["entries"].is_array())
    {
        for (const json& entry : message["entries"])
        {
            if (result.entries.size() >= MostResultEntries) break;
            if (entry.is_string()) result.entries.push_back(utf8::Prefix(entry.get<std::string>(), 2000));
        }
    }
    outResult = std::move(result);
    return true;
}

bool ReadRefusal(const std::string& line, std::string& outReason)
{
    json message;
    std::string error;
    if (!Parse(line, message, error) || Text(message, "type") != "refused") return false;
    outReason = Text(message, "reason");
    return true;
}

} // namespace revia::stage
