#include "Learning/liveCapture.h"

#include "Audit/contentDigest.h"
#include "Core/atomicFile.h"
#include "Identity/promptMarkers.h"
#include "Memory/sensitiveContent.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace revia::learning
{
namespace
{
using nlohmann::json;
namespace markers = identity::markers;

// The exporter's own bounds, so a capture it would exclude is refused before the owner
// spends a correction on it.
constexpr std::size_t MaximumMessages = 256;
constexpr std::size_t MaximumText = 64 * 1024;

// The request fields that shape generation, as revia_dataset_capture.py keeps them.
constexpr std::array<std::string_view, 20> Settings = {"model", "temperature", "top_p", "top_k", "min_p", "max_tokens",
    "presence_penalty", "frequency_penalty", "seed", "dry_multiplier", "dry_base", "dry_allowed_length", "dry_penalty_last_n",
    "chat_template_kwargs", "reasoning_budget_tokens", "stream", "stop", "response_format", "tool_choice", "parallel_tool_calls"};

// Private reference material whose sources the runtime does not name yet, so a capture of it
// could not be withdrawn when they are forgotten. Saved memories are not here: they are
// revised, never deleted.
constexpr std::array<std::pair<std::string_view, std::string_view>, 6> UnnamedReferences = {{
    {markers::RetrievedConversationBlock, "conversation history recalled from other chats"},
    {markers::AttachmentGrounding, "attached files"},
    {markers::DocumentGrounding, "passages from kept documents"},
    {markers::ClipboardGrounding, "the clipboard"},
    {markers::LivePageGrounding, "live page text"},
    {markers::VisibleBrowserGrounding, "visible-browser results"},
}};

json StampJson(const runtime::RuntimeStamp& value)
{
    return {{"companionId", value.companionId}, {"sessionId", value.sessionId}, {"generation", value.generation},
        {"taskId", value.taskId}, {"attemptId", value.attemptId}, {"policyVersion", value.policyVersion}};
}

// The judgment in the names the candidate interchange reads.
json JudgmentJson(const QualityFeedback& value)
{
    const auto issue = [&]
    {
        switch (value.issue)
        {
        case QualityIssue::AnswerCoverage:
            return "answer-coverage";
        case QualityIssue::UnsupportedClaim:
            return "unsupported-claim";
        case QualityIssue::MemoryContinuity:
            return "memory-continuity";
        case QualityIssue::MissingAcceptanceEvidence:
            return "missing-acceptance-evidence";
        }
        return "unsupported";
    }();
    return {{"source", "owner-judgment"}, {"target", value.target == QualityTarget::ConversationTurn ? "conversation-turn" : "agent-attempt"},
        {"issue", issue}, {"judgmentId", value.judgmentId}, {"sourceId", value.sourceId}, {"targetId", value.targetId},
        {"targetDigest", value.targetDigest}, {"criterion", value.criterion}, {"evidence", value.evidence},
        {"dependencies", value.dependencies}, {"correctsRecordId", value.correctsRecordId}, {"criterionSatisfied", false},
        {"privateContextExcluded", false}, {"audienceRevision", value.audienceRevision}, {"origin", StampJson(value.origin)}};
}

bool Refuse(std::string& error, std::string reason)
{
    error = std::move(reason);
    return false;
}

bool Build(const DeliveredReply& reply, const QualityFeedback& judgment, LiveCapture& outCapture, std::string& outError)
{
    std::string error;
    if (judgment.source != QualityEvidenceSource::OwnerJudgment || judgment.criterionSatisfied ||
        judgment.target != QualityTarget::ConversationTurn || !ValidateQualityFeedback(judgment, error))
        return Refuse(outError, "Only an owner's judgment that a conversation reply failed can keep its exchange.");
    if (!judgment.origin.SameSession(reply.origin) || judgment.audienceRevision != reply.audienceRevision ||
        reply.displayed.empty() || judgment.targetDigest != audit::ContentDigest(reply.displayed))
        return Refuse(outError, "The judgment is not bound to this delivered reply.");
    if (reply.replaced)
        return Refuse(outError, "The displayed reply replaced the model's answer, so it is not what this request produced.");
    if (!reply.exchange)
        return Refuse(outError, "No local model exchange is held for this reply.");
    const auto& exchange = *reply.exchange;
    const auto dependency = [&judgment](const char* name)
    {
        const auto found = judgment.dependencies.find(name);
        return found == judgment.dependencies.end() ? std::string{} : found->second;
    };
    if (reply.profileDigest.empty() || reply.configurationDigest.empty() || dependency("loaded-profile-sha256") != reply.profileDigest ||
        dependency("effective-llm-config-sha256") != reply.configurationDigest)
        return Refuse(outError, "The profile or model configuration changed after this reply; judge a fresh reply instead.");
    if (exchange.model.empty() || exchange.templateSha256.empty())
        return Refuse(outError, "The served model or chat template is unknown, so the exchange cannot be kept exactly.");
    if (exchange.requestedTools)
        return Refuse(outError, "The reply asked for tool calls, which the text dataset cannot hold.");

    const json request = json::parse(exchange.requestBody, nullptr, false);
    if (!request.is_object() || !request.contains("messages") || !request["messages"].is_array() || request["messages"].empty())
        return Refuse(outError, "The held request has no messages.");
    json prompt = json::array();
    std::string promptText;
    for (const auto& message : request["messages"])
    {
        const std::string role = message.value("role", "");
        if (role != "system" && role != "user" && role != "assistant" && role != "tool")
            return Refuse(outError, "The prompt has a message role the dataset does not hold.");
        if (!message.contains("content") || !message["content"].is_string())
            return Refuse(outError, "The prompt contains image or other non-text parts, which the text dataset cannot hold exactly.");
        if (message.contains("tool_calls") || role == "tool")
            return Refuse(outError, "The prompt contains tool-call history, which the text dataset cannot hold exactly.");
        const auto& content = message["content"].get_ref<const std::string&>();
        if (content.empty() || content.size() > MaximumText)
            return Refuse(outError, "A prompt message is empty or exceeds the dataset's bound.");
        prompt.push_back({{"role", role}, {"content", content}});
        promptText += content + '\n';
    }
    if (prompt.size() > MaximumMessages || prompt.back()["role"] != "user")
        return Refuse(outError, "The prompt exceeds the dataset's bounds or does not end with the turn being answered.");
    const auto kwargs = request.find("chat_template_kwargs");
    if (kwargs == request.end() || !kwargs->is_object() || !kwargs->contains("enable_thinking") ||
        !(*kwargs)["enable_thinking"].is_boolean() || (*kwargs)["enable_thinking"].get<bool>())
        return Refuse(outError, "The reply was not generated with thinking off, which the dataset cannot reproduce.");
    for (const auto& [marker, description] : UnnamedReferences)
        if (promptText.find(marker) != std::string::npos)
            return Refuse(outError, "The prompt carries " + std::string(description) +
                                        ", whose sources the runtime cannot yet withdraw from a capture.");
    json tools;
    if (const auto offered = request.find("tools"); offered != request.end())
    {
        if (!offered->is_array() || offered->empty() ||
            !std::all_of(offered->begin(), offered->end(),
                [](const json& tool)
                {
                    return tool.is_object() && tool.value("type", "") == "function" && tool.contains("function") &&
                        tool["function"].is_object() && tool["function"].contains("name") && tool["function"]["name"].is_string() &&
                        !tool["function"]["name"].get_ref<const std::string&>().empty();
                }))
            return Refuse(outError, "The request offered tools that are not named functions.");
        tools = *offered;
    }
    if (reply.displayed.size() > MaximumText)
        return Refuse(outError, "The displayed reply exceeds the dataset's bound.");
    if (const auto finding = memory::DetectSensitiveContent(
            promptText + (tools.is_null() ? std::string{} : tools.dump()) + '\n' + reply.displayed + '\n' + exchange.responseText))
        return Refuse(outError, "The exchange contains " + finding.description + ", so it is not written anywhere.");

    json settings = json::object();
    for (const auto key : Settings)
        if (const auto found = request.find(std::string(key)); found != request.end())
            settings[std::string(key)] = *found;
    const std::string requestDigest = audit::ContentDigest(exchange.requestBody);
    outCapture.captureId = audit::ContentDigest(prompt.dump() + '\n' + reply.displayed).substr(0, 16);
    outCapture.sources = {"request:" + requestDigest};
    if (!reply.conversationId.empty())
        outCapture.sources.push_back("conversation:" + reply.conversationId);
    if (!reply.participantId.empty())
        outCapture.sources.push_back("participant:" + reply.participantId);

    json record = {{"captureId", outCapture.captureId},
        {"source", {{"kind", "live"}, {"version", LiveCaptureVersion}, {"requestBodySha256", requestDigest},
                       {"conversationId", reply.conversationId}, {"participantId", reply.participantId}}},
        {"origin", StampJson(reply.origin)}, {"judgment", JudgmentJson(judgment)}, {"sources", outCapture.sources}, {"prompt", prompt},
        {"failedOutput", reply.displayed}, {"failedOutputSha256", judgment.targetDigest},
        // The text the model streamed, kept beside the judged reply when style repair changed it.
        {"modelOutput", exchange.responseText}, {"modelOutputSha256", audit::ContentDigest(exchange.responseText)},
        {"repaired", reply.modelText != reply.displayed}, {"finishReason", exchange.finishReason},
        {"completeStream", exchange.completeStream}, {"requestSettings", settings},
        {"provenance", {{"model", exchange.model}, {"profileDigest", reply.profileDigest}, {"templateSha256", exchange.templateSha256},
                           {"configurationDigest", reply.configurationDigest}}},
        {"context", {{"input", prompt.back()["content"]}, {"failures", json::array({judgment.criterion})}}}};
    if (!tools.is_null())
        record["tools"] = std::move(tools);
    outCapture.record = record.dump(2) + '\n';
    return true;
}
} // namespace

bool BuildLiveCapture(const DeliveredReply& reply, const QualityFeedback& judgment, LiveCapture& outCapture, std::string& outError)
{
    outCapture = {};
    outError.clear();
    try
    {
        if (Build(reply, judgment, outCapture, outError))
            return true;
    }
    catch (const json::exception&)
    {
        outError = "The held request or reply is not well-formed JSON text.";
    }
    outCapture = {};
    return false;
}

LiveCaptureStore::LiveCaptureStore(std::filesystem::path value) : root(std::move(value)) {}

std::filesystem::path LiveCaptureStore::LiveDirectory() const
{
    return root / "live";
}

std::filesystem::path LiveCaptureStore::WithdrawnList() const
{
    return root / "withdrawn.json";
}

bool LiveCaptureStore::Keep(const LiveCapture& capture, std::string& outError) const
{
    outError.clear();
    if (capture.captureId.size() != 16 ||
        !std::all_of(capture.captureId.begin(), capture.captureId.end(), [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        capture.record.empty())
    {
        outError = "The capture has no identity or content.";
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(LiveDirectory(), error);
    const auto target = LiveDirectory() / (capture.captureId + ".json");
    if (error || std::filesystem::exists(target, error))
    {
        outError = error ? "The capture directory could not be created: " + error.message()
                         : "This reply's exchange is already kept as capture " + capture.captureId + ".";
        return false;
    }
    auto partial = target;
    partial += ".partial";
    {
        std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
        stream << capture.record;
        if (!stream.flush())
        {
            stream.close();
            std::filesystem::remove(partial, error);
            outError = "The capture could not be written.";
            return false;
        }
    }
    // A rename never replaces here: the target was checked absent under the caller's lock.
    std::filesystem::rename(partial, target, error);
    if (error)
    {
        std::filesystem::remove(partial, error);
        outError = "The capture could not be moved into place.";
        return false;
    }
    return true;
}

bool LiveCaptureStore::Withdraw(const std::string& source, std::size_t& outRemoved, std::string& outError) const
{
    return WithdrawMatching(&source, outRemoved, outError);
}

bool LiveCaptureStore::WithdrawAll(std::size_t& outRemoved, std::string& outError) const
{
    return WithdrawMatching(nullptr, outRemoved, outError);
}

bool LiveCaptureStore::WithdrawMatching(const std::string* source, std::size_t& outRemoved, std::string& outError) const
{
    outRemoved = 0;
    outError.clear();
    std::error_code error;
    if (!std::filesystem::exists(root, error))
        return true;
    std::set<std::string> withdrawn;
    const auto list = WithdrawnList();
    try
    {
        if (std::filesystem::exists(list, error))
        {
            std::ifstream stream(list, std::ios::binary);
            const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            for (const auto& item : json::parse(bytes).get<std::vector<std::string>>())
                withdrawn.insert(item);
        }
        if (source)
            withdrawn.insert(*source);
        std::vector<std::filesystem::path> matched;
        if (std::filesystem::is_directory(LiveDirectory(), error))
            for (const auto& entry : std::filesystem::directory_iterator(LiveDirectory()))
            {
                if (entry.path().extension() != ".json")
                    continue;
                std::ifstream stream(entry.path(), std::ios::binary);
                const auto record = json::parse(std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()});
                const auto sources = record.at("sources").get<std::vector<std::string>>();
                if (source && std::find(sources.begin(), sources.end(), *source) == sources.end())
                    continue;
                // Each capture's own request, and for a withdrawal of everything its conversation too.
                // A speaker is not added: other conversations with them stay kept.
                for (const auto& name : sources)
                    if (name.starts_with("request:") || (!source && name.starts_with("conversation:")))
                        withdrawn.insert(name);
                matched.push_back(entry.path());
            }
        // The list is written first, so a crash between the two leaves a capture that is already refused.
        auto partial = list;
        partial += ".partial";
        {
            std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
            stream << json(std::vector<std::string>(withdrawn.begin(), withdrawn.end())).dump(2) << '\n';
            if (!stream.flush())
                throw std::runtime_error("the withdrawal list could not be written");
        }
        if (!core::ReplaceFileAtomically(list, partial, outError))
            return false;
        for (const auto& path : matched)
        {
            if (!std::filesystem::remove(path, error) || error)
            {
                outError = "A withdrawn capture could not be deleted; it is already listed as withdrawn.";
                return false;
            }
            ++outRemoved;
        }
        return true;
    }
    catch (const std::exception&)
    {
        outError = "The kept captures or their withdrawal list could not be read, so nothing was withdrawn.";
        return false;
    }
}

} // namespace revia::learning
