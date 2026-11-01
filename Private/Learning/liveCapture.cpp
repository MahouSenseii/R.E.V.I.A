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
#include <optional>
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

constexpr const char* CorrectionSchema = "revia-live-correction-v1";
// Earlier revisions kept in a correction file; older ones are let go.
constexpr std::size_t MaximumCorrectionHistory = 32;

// The candidate interchange's names, which revia_dataset_capture.py reads back.
std::string DecisionName(const LearningDecision value)
{
    switch (value)
    {
    case LearningDecision::Pending:
        return "pending";
    case LearningDecision::Accept:
        return "accept";
    case LearningDecision::Rework:
        return "rework";
    case LearningDecision::Reject:
        return "reject";
    case LearningDecision::NeedEvidence:
        return "need-evidence";
    }
    return "pending";
}

std::optional<LearningDecision> ParseDecision(const std::string& value)
{
    for (const auto decision : {LearningDecision::Pending, LearningDecision::Accept, LearningDecision::Rework, LearningDecision::Reject,
             LearningDecision::NeedEvidence})
        if (DecisionName(decision) == value)
            return decision;
    return std::nullopt;
}

// NUL is outside permanent source identifier grammar; JSON preserves it by length.
const std::string PendingCleanupPrefix = std::string(1, '\0') + "revia-live-capture-cleanup-v1:";

std::string PendingCleanup(const std::string& captureId)
{
    return PendingCleanupPrefix + captureId;
}

bool CaptureId(const std::string& value)
{
    return value.size() == 16 &&
        std::all_of(value.begin(), value.end(), [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// The person's own words: the newest message can open with the runtime's per-turn context.
std::string PersonsWords(const std::string& content)
{
    if (!content.starts_with(markers::RuntimeTurnContext))
        return content;
    const auto end = content.find(markers::RuntimeTurnContextEnd);
    if (end == std::string::npos)
        return content;
    const auto begin = content.find_first_not_of("\r\n", end + markers::RuntimeTurnContextEnd.size());
    return begin == std::string::npos ? std::string{} : content.substr(begin);
}

// Writes bytes through a sibling .partial file, so a reader sees the old file or the new one.
bool WriteReplacing(const std::filesystem::path& target, const std::string& bytes, std::string& error)
{
    auto partial = target;
    partial += ".partial";
    {
        std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
        stream << bytes;
        if (!stream.flush())
        {
            error = "The file could not be written.";
            return false;
        }
    }
    return core::ReplaceFileAtomically(target, partial, error);
}

bool RemoveCorrection(const std::filesystem::path& directory, const std::string& captureId, std::size_t& removed)
{
    bool completed = true;
    for (const char* suffix : {".json", ".json.bak", ".json.partial"})
    {
        std::error_code error;
        if (std::filesystem::remove(directory / (captureId + suffix), error))
            ++removed;
        completed = completed && !error;
    }
    return completed;
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
    // What the input came from. The speaker is recorded beside them, not among them: as a source
    // it would join every capture of one person into one dataset group.
    outCapture.sources = {"request:" + requestDigest};
    if (!reply.conversationId.empty())
        outCapture.sources.push_back("conversation:" + reply.conversationId);

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

std::filesystem::path LiveCaptureStore::CorrectionsDirectory() const
{
    return root / "corrections";
}

std::filesystem::path LiveCaptureStore::WithdrawnList() const
{
    return root / "withdrawn.json";
}

std::string LiveCaptureStore::Revision() const
{
    std::string revision;
    for (const auto& directory : {LiveDirectory(), CorrectionsDirectory()})
    {
        std::error_code error;
        const auto written = std::filesystem::last_write_time(directory, error);
        revision += error ? std::string("none") : std::to_string(written.time_since_epoch().count());
        revision += '/';
    }
    return revision;
}

std::vector<LiveCaptureSummary> LiveCaptureStore::List(std::string& outError) const
{
    outError.clear();
    std::vector<std::pair<std::filesystem::file_time_type, LiveCaptureSummary>> found;
    std::size_t unreadable = 0;
    std::error_code error;
    if (!std::filesystem::is_directory(LiveDirectory(), error))
        return {};
    for (const auto& entry : std::filesystem::directory_iterator(LiveDirectory(), error))
    {
        if (entry.path().extension() != ".json")
            continue;
        try
        {
            const auto record = json::parse(ReadText(entry.path()));
            LiveCaptureSummary summary;
            summary.captureId = record.at("captureId").get<std::string>();
            if (!CaptureId(summary.captureId) || entry.path().stem().string() != summary.captureId)
                throw std::runtime_error("name");
            const auto& prompt = record.at("prompt");
            if (!prompt.is_array() || prompt.empty())
                throw std::runtime_error("prompt");
            summary.input = PersonsWords(prompt.back().at("content").get<std::string>());
            summary.failedOutput = record.at("failedOutput").get<std::string>();
            summary.failedOutputSha256 = record.at("failedOutputSha256").get<std::string>();
            summary.modelOutput = record.at("modelOutput").get<std::string>();
            summary.repaired = record.at("repaired").get<bool>();
            const auto& judgment = record.at("judgment");
            summary.issue = judgment.at("issue").get<std::string>();
            summary.criterion = judgment.at("criterion").get<std::string>();
            summary.evidence = judgment.at("evidence").get<std::string>();
            summary.conversationId = record.at("source").value("conversationId", "");
            const auto correction = CorrectionsDirectory() / (summary.captureId + ".json");
            if (std::filesystem::exists(correction, error))
            {
                const auto saved = json::parse(ReadText(correction));
                const auto decision = ParseDecision(saved.at("decision").get<std::string>());
                if (!decision || saved.at("captureId") != summary.captureId || saved.at("failedOutputSha256") != summary.failedOutputSha256)
                    throw std::runtime_error("correction");
                summary.decision = *decision;
                summary.correction = saved.at("correction").get<std::string>();
                summary.revision = saved.at("revision").get<std::uint64_t>();
            }
            found.emplace_back(entry.last_write_time(error), std::move(summary));
        }
        catch (const std::exception&)
        {
            ++unreadable;
        }
    }
    std::sort(found.begin(), found.end(),
        [](const auto& left, const auto& right)
        { return left.first != right.first ? left.first > right.first : left.second.captureId < right.second.captureId; });
    std::vector<LiveCaptureSummary> summaries;
    for (auto& item : found)
        summaries.push_back(std::move(item.second));
    if (unreadable > 0)
        outError = std::to_string(unreadable) + (unreadable == 1 ? " kept capture or its correction could not be read and is not shown."
                                                                  : " kept captures or their corrections could not be read and are not shown.");
    return summaries;
}

bool LiveCaptureStore::CandidateFile(
    std::string& outJson, std::size_t& outCandidates, std::size_t& outWaiting, std::string& outError) const
{
    outJson.clear();
    outCandidates = 0;
    outWaiting = 0;
    outError.clear();
    std::error_code error;
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(LiveDirectory(), error))
        for (const auto& entry : std::filesystem::directory_iterator(LiveDirectory(), error))
            if (entry.path().extension() == ".json")
                files.push_back(entry.path());
    // The capture tool reads them in name order.
    std::sort(files.begin(), files.end());
    json candidates = json::array();
    try
    {
        for (const auto& path : files)
        {
            const auto capture = json::parse(ReadText(path));
            const auto captureId = capture.at("captureId").get<std::string>();
            if (!CaptureId(captureId) || path.stem().string() != captureId || capture.at("source").at("kind") != "live")
                return Refuse(outError, path.filename().string() + " in the captures folder is not a live capture.");
            const auto correctionPath = CorrectionsDirectory() / (captureId + ".json");
            if (!std::filesystem::exists(correctionPath, error))
            {
                ++outWaiting;
                continue;
            }
            const auto saved = json::parse(ReadText(correctionPath));
            const auto& failedDigest = capture.at("failedOutputSha256");
            if (saved.at("schema") != CorrectionSchema || saved.at("captureId") != captureId || saved.at("failedOutputSha256") != failedDigest)
                return Refuse(outError, "The saved correction of capture " + captureId + " belongs to another reply.");
            const auto decision = ParseDecision(saved.at("decision").get<std::string>());
            if (!decision)
                return Refuse(outError, "Capture " + captureId + " has an unknown decision.");
            if (*decision == LearningDecision::Pending)
            {
                ++outWaiting;
                continue;
            }
            const auto failed = capture.at("failedOutput").get<std::string>();
            const auto& judgment = capture.at("judgment");
            if (judgment.at("targetDigest") != failedDigest || failedDigest != audit::ContentDigest(failed))
                return Refuse(outError, "Capture " + captureId + "'s recorded judgment is not bound to its reply.");
            const auto correction = saved.at("correction").get<std::string>();
            if (*decision == LearningDecision::Accept && (correction.find_first_not_of(" \t\r\n") == std::string::npos || correction == failed))
                return Refuse(outError, "Capture " + captureId + ": an accepted capture needs a correction that differs from the failed reply.");
            const auto& provenance = capture.at("provenance");
            candidates.push_back({{"exampleId", "live:" + captureId}, {"origin", capture.at("origin")}, {"sourceIds", capture.at("sources")},
                {"prompt", capture.at("prompt")}, {"tools", capture.value("tools", json::array())}, {"response", correction},
                {"targetOrigin", "reviewed-correction"}, {"rejectedResponse", failed}, {"judgment", judgment},
                {"review", DecisionName(*decision)}, {"modelId", provenance.at("model")},
                {"templateId", "chat-template-sha256:" + provenance.at("templateSha256").get<std::string>()},
                {"transformationVersion", LiveCaptureVersion}});
        }
    }
    catch (const std::exception&)
    {
        return Refuse(outError, "A kept capture or its saved correction could not be read, so nothing was exported.");
    }
    outCandidates = candidates.size();
    outJson = json{{"schema", "revia-dataset-candidates-v1"}, {"candidates", std::move(candidates)}}.dump();
    return true;
}

bool LiveCaptureStore::Correct(const std::string& captureId, const LearningDecision decision, const std::string& correction,
    std::uint64_t& outRevision, std::string& outError) const
{
    outRevision = 0;
    outError.clear();
    std::error_code error;
    const auto capturePath = LiveDirectory() / (captureId + ".json");
    if (!CaptureId(captureId) || !std::filesystem::exists(capturePath, error))
        return Refuse(outError, "That capture is no longer kept.");
    if (decision == LearningDecision::Pending)
        return Refuse(outError, "Choose a decision before saving.");
    if (correction.size() > MaximumText || correction.find('\0') != std::string::npos)
        return Refuse(outError, "The correction exceeds the dataset's bound.");
    try
    {
        const auto record = json::parse(ReadText(capturePath));
        const auto failed = record.at("failedOutput").get<std::string>();
        const auto failedDigest = record.at("failedOutputSha256").get<std::string>();
        if (decision == LearningDecision::Accept &&
            (correction.find_first_not_of(" \t\r\n") == std::string::npos || correction == failed))
            return Refuse(outError, "An accepted capture needs a correct reply that differs from the judged one.");
        if (const auto finding = memory::DetectSensitiveContent(correction))
            return Refuse(outError, "The correction contains " + finding.description + ", so it was not saved.");
        const auto target = CorrectionsDirectory() / (captureId + ".json");
        json history = json::array();
        std::uint64_t revision = 1;
        if (std::filesystem::exists(target, error))
        {
            const auto previous = json::parse(ReadText(target));
            if (previous.at("captureId") != captureId || previous.at("failedOutputSha256") != failedDigest)
                return Refuse(outError, "The saved correction belongs to another capture, so it was not replaced.");
            history = previous.at("history");
            history.push_back({{"revision", previous.at("revision")}, {"decision", previous.at("decision")},
                {"correction", previous.at("correction")}});
            if (history.size() > MaximumCorrectionHistory)
                history.erase(history.begin(), history.end() - static_cast<std::ptrdiff_t>(MaximumCorrectionHistory));
            revision = previous.at("revision").get<std::uint64_t>() + 1;
        }
        const json saved = {{"schema", CorrectionSchema}, {"captureId", captureId}, {"failedOutputSha256", failedDigest},
            {"revision", revision}, {"decision", DecisionName(decision)}, {"correction", correction}, {"history", history}};
        std::filesystem::create_directories(CorrectionsDirectory(), error);
        if (error)
            return Refuse(outError, "The corrections directory could not be created: " + error.message());
        if (!WriteReplacing(target, saved.dump(2) + '\n', outError))
            return false;
        outRevision = revision;
        return true;
    }
    catch (const std::exception&)
    {
        return Refuse(outError, "The capture or its saved correction could not be read, so nothing was saved.");
    }
}

bool LiveCaptureStore::Discard(const std::string& captureId, std::string& outError) const
{
    outError.clear();
    if (!CaptureId(captureId))
        return Refuse(outError, "That capture is no longer kept.");
    const auto result = WithdrawMatching(nullptr, &captureId);
    outError = result.error;
    if (!result.succeeded)
        return false;
    return result.pendingCleanupCompleted || result.capturesRemoved > 0 || result.correctionsRemoved > 0 ||
           Refuse(outError, "That capture is no longer kept.");
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
    try
    {
        std::error_code inspection;
        const bool listed = std::filesystem::exists(WithdrawnList(), inspection);
        if (inspection)
            return Refuse(outError, "The withdrawal list could not be inspected, so the capture was not kept.");
        if (listed)
            for (const auto& name : json::parse(ReadText(WithdrawnList())).get<std::vector<std::string>>())
                if (name == PendingCleanup(capture.captureId))
                    return Refuse(outError, "This capture has cleanup pending. Retry its withdrawal before keeping it again.");
    }
    catch (const std::exception&)
    {
        return Refuse(outError, "The withdrawal list could not be read, so the capture was not kept.");
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

CaptureWithdrawalResult LiveCaptureStore::Withdraw(const std::string& source) const
{
    if (source.find('\0') != std::string::npos)
    {
        CaptureWithdrawalResult result;
        result.succeeded = false;
        result.error = "A withdrawal source cannot contain operational cleanup metadata.";
        return result;
    }
    return WithdrawMatching(&source, nullptr);
}

CaptureWithdrawalResult LiveCaptureStore::WithdrawAll() const
{
    return WithdrawMatching(nullptr, nullptr);
}

CaptureWithdrawalResult LiveCaptureStore::WithdrawMatching(const std::string* source, const std::string* onlyCapture) const
{
    CaptureWithdrawalResult result;
    const auto fail = [&result](const std::string& error)
    {
        result.succeeded = false;
        result.error = error.substr(0, 384);
        return result;
    };
    std::error_code error;
    const bool present = std::filesystem::exists(root, error);
    if (error)
        return fail("The kept capture store could not be inspected.");
    if (!present)
        return result;
    std::set<std::string> withdrawn;
    const auto list = WithdrawnList();
    try
    {
        const bool listed = std::filesystem::exists(list, error);
        if (error)
            return fail("The withdrawal list could not be inspected; nothing was withdrawn.");
        if (listed)
            for (const auto& item : json::parse(ReadText(list)).get<std::vector<std::string>>())
                withdrawn.insert(item);
        std::set<std::string> matched;
        // These exact IDs were durably withdrawn before a previous deletion attempt.
        for (const auto& name : withdrawn)
            if (name.starts_with(PendingCleanupPrefix))
            {
                const auto id = name.substr(PendingCleanupPrefix.size());
                if (CaptureId(id) && (!onlyCapture || id == *onlyCapture))
                    matched.insert(id);
            }
        const bool hadPendingCleanup = !matched.empty();
        if (source)
            withdrawn.insert(*source);
        const bool live = std::filesystem::exists(LiveDirectory(), error);
        if (error || (live && !std::filesystem::is_directory(LiveDirectory())))
            return fail("The kept captures could not be inspected; nothing was withdrawn.");
        if (live)
            for (const auto& entry : std::filesystem::directory_iterator(LiveDirectory()))
            {
                if (entry.path().extension() != ".json")
                    continue;
                const auto id = entry.path().stem().string();
                if (onlyCapture && id != *onlyCapture)
                    continue;
                const auto record = json::parse(ReadText(entry.path()));
                if (!CaptureId(id) || record.at("captureId") != id)
                    return fail("A kept capture has an invalid identity; nothing was withdrawn.");
                const auto sources = record.at("sources").get<std::vector<std::string>>();
                const bool named = source && (source->starts_with("participant:")
                    ? "participant:" + record.at("source").value("participantId", "") == *source
                    : std::find(sources.begin(), sources.end(), *source) != sources.end());
                if (!onlyCapture && source && !named && !matched.contains(id))
                    continue;
                const auto target = record.at("judgment").at("targetId").get<std::string>();
                if (target.find('\0') != std::string::npos)
                    return fail("A capture has an invalid withdrawal source; cleanup was not started.");
                withdrawn.insert(target);
                withdrawn.insert(PendingCleanup(id));
                if (!source && !onlyCapture)
                    for (const auto& name : sources)
                        if (name.starts_with("conversation:"))
                            withdrawn.insert(name);
                matched.insert(id);
            }
        const bool corrections = std::filesystem::exists(CorrectionsDirectory(), error);
        if (error || (corrections && !std::filesystem::is_directory(CorrectionsDirectory())))
            return fail("The kept corrections could not be inspected; nothing was withdrawn.");
        if (corrections)
            for (const auto& entry : std::filesystem::directory_iterator(CorrectionsDirectory()))
            {
                const auto name = entry.path().filename().string();
                std::string id;
                for (const std::string suffix : {".json", ".json.bak", ".json.partial"})
                    if (name.ends_with(suffix))
                        id = name.substr(0, name.size() - suffix.size());
                if (!CaptureId(id) || (onlyCapture && id != *onlyCapture))
                    continue;
                const bool capturePresent = std::filesystem::exists(LiveDirectory() / (id + ".json"));
                if (matched.contains(id) || (!source && (!capturePresent || onlyCapture)))
                {
                    matched.insert(id);
                    withdrawn.insert(PendingCleanup(id));
                }
                else if (!capturePresent && source)
                    return fail("A correction without its original capture has unconfirmed source scope. Explicit forget-all can remove it; targeted withdrawal left it intact.");
            }
        // Publish every withdrawal and retry identity before removing any private bytes.
        auto partial = list;
        partial += ".partial";
        {
            std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
            stream << json(std::vector<std::string>(withdrawn.begin(), withdrawn.end())).dump(2) << '\n';
            if (!stream.flush())
                return fail("The withdrawal list could not be written; no captures or corrections were deleted.");
        }
        std::string replacementError;
        if (!core::ReplaceFileAtomically(list, partial, replacementError))
            return fail("The withdrawal list could not be published; no captures or corrections were deleted. " + replacementError);
        result.withdrawalRecorded = true;
        for (const auto& id : matched)
        {
            error.clear();
            if (std::filesystem::remove(LiveDirectory() / (id + ".json"), error))
                ++result.capturesRemoved;
            if (error)
                return fail("Withdrawn capture " + id + " could not be deleted; its withdrawal is recorded. Retry the same withdrawal.");
            if (!RemoveCorrection(CorrectionsDirectory(), id, result.correctionsRemoved))
                return fail("Withdrawn capture " + id + " has a correction that could not be deleted; its withdrawal is recorded. Retry the same withdrawal.");
        }
        for (const auto& id : matched)
            withdrawn.erase(PendingCleanup(id));
        if (!matched.empty() && !WriteReplacing(list, json(std::vector<std::string>(withdrawn.begin(), withdrawn.end())).dump(2) + '\n', replacementError))
            return fail("Capture and correction deletion completed, but cleanup finalization is pending. Retry the same withdrawal.");
        result.pendingCleanupCompleted = hadPendingCleanup;
        return result;
    }
    catch (const std::exception&)
    {
        return fail(result.withdrawalRecorded
            ? "Withdrawal was recorded, but capture/correction cleanup did not complete. Retry the same withdrawal."
            : "The kept captures or their withdrawal list could not be read, so nothing was withdrawn.");
    }
}

} // namespace revia::learning
