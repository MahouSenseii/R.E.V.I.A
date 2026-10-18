#include "LLM/Cloud/cloudChatService.h"

#include "Audit/contentDigest.h"
#include "Core/utf8.h"
#include "LLM/Cloud/cloudProtocol.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace revia::llm::cloud
{

namespace
{
double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::optional<std::string> ProcessEnvironment(const std::string& name)
{
    const char* value = std::getenv(name.c_str());
    if (value == nullptr)
        return std::nullopt;
    return std::string(value);
}

std::string Trimmed(const std::string& text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// A reply cut off by the token limit keeps its complete sentences, as a local one does.
std::string CompleteSentences(const std::string& text)
{
    const std::size_t end = text.find_last_of(".!?");
    return end == std::string::npos ? std::string{} : text.substr(0, end + 1);
}

responseOutput Refusal(const std::string& response, const std::string& reason)
{
    responseOutput output;
    output.bSuccess = false;
    output.response = response;
    output.reason = reason;
    output.bShouldSpeak = true;
    output.bShouldRemember = false;
    output.selectedTier = "Cloud";
    return output;
}

// The provider's own explanation of an HTTP error, bounded. The body is the server's, not
// the conversation's, and is shown only as a reason.
std::string ErrorPreview(const std::string& body)
{
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    std::string message;
    if (parsed.is_object() && parsed.contains("error"))
    {
        const auto& error = parsed["error"];
        if (error.is_string())
            message = error.get<std::string>();
        else if (error.is_object() && error.contains("message") && error["message"].is_string())
            message = error["message"].get<std::string>();
    }
    if (message.empty())
        message = body;
    message = revia::utf8::Prefix(message, 300);
    return revia::utf8::IsValid(message) ? message : std::string("[error body was not valid UTF-8]");
}
} // namespace

CloudChatService::CloudChatService()
{
    state.transport = MakePlatformCloudTransport();
    state.environment = ProcessEnvironment;
}

CloudChatService::~CloudChatService() = default;

void CloudChatService::ApplySettings(const CloudModelSettings& settings)
{
    std::lock_guard lock(mutex);
    state.settings = settings;
}

void CloudChatService::SetPersona(std::string systemPrompt)
{
    std::lock_guard lock(mutex);
    state.persona = std::move(systemPrompt);
}

void CloudChatService::SetTransport(std::shared_ptr<CloudTransport> transport)
{
    std::lock_guard lock(mutex);
    state.transport = std::move(transport);
}

void CloudChatService::SetEnvironmentReader(EnvironmentReader reader)
{
    std::lock_guard lock(mutex);
    state.environment = std::move(reader);
}

void CloudChatService::SetAuditSink(AuditSink sink)
{
    std::lock_guard lock(mutex);
    state.audit = std::move(sink);
}

void CloudChatService::SetSessionEnabled(const bool enabled)
{
    std::lock_guard lock(mutex);
    state.sessionEnabled = enabled;
}

bool CloudChatService::SessionEnabled() const
{
    std::lock_guard lock(mutex);
    return state.sessionEnabled;
}

CloudModelSettings CloudChatService::Settings() const
{
    std::lock_guard lock(mutex);
    return state.settings;
}

CloudUse CloudChatService::Use() const
{
    std::lock_guard lock(mutex);
    return ParseCloudUse(state.settings.useFor);
}

CloudChatService::Snapshot CloudChatService::Capture() const
{
    std::lock_guard lock(mutex);
    return state;
}

CloudAvailability CloudChatService::Check(const Snapshot& snapshot, std::string* key)
{
    CloudAvailability availability;
    const CloudModelSettings& settings = snapshot.settings;
    if (!settings.bEnabled)
    {
        availability.reason = "The cloud model is off (llm.cloud.enabled is false in Config/settings.json).";
        return availability;
    }
    if (ParseCloudUse(settings.useFor) == CloudUse::Never)
    {
        // "never" means never, explicit asks included.
        availability.reason = "llm.cloud.useFor is \"never\"; set it to \"ask\" or \"hardTurns\" to use the cloud model.";
        return availability;
    }
    if (!snapshot.sessionEnabled)
    {
        availability.reason = "The cloud model is switched off for this session. /cloud on allows it again.";
        return availability;
    }
    if (!ParseCloudProvider(settings.provider))
    {
        availability.reason = "llm.cloud.provider must be \"openai-compatible\" or \"anthropic\".";
        return availability;
    }
    if (Trimmed(settings.model).empty())
    {
        availability.reason = "llm.cloud.model names no model.";
        return availability;
    }
    std::string endpointError;
    if (!ParseCloudEndpoint(settings.baseUrl, endpointError))
    {
        availability.reason = endpointError;
        return availability;
    }
    if (Trimmed(settings.apiKeyEnvironmentVariable).empty())
    {
        availability.reason = "llm.cloud.apiKeyEnvironmentVariable names no environment variable to read the key from.";
        return availability;
    }
    const std::optional<std::string> value =
        snapshot.environment ? snapshot.environment(settings.apiKeyEnvironmentVariable) : std::nullopt;
    if (!value || Trimmed(*value).empty())
    {
        availability.reason = "No key: the environment variable " + settings.apiKeyEnvironmentVariable + " is not set.";
        return availability;
    }
    if (!snapshot.audit)
    {
        availability.reason = "Cloud requests cannot be recorded in the action audit here, so none are sent.";
        return availability;
    }
    if (!snapshot.transport)
    {
        availability.reason = "No HTTPS transport is available for cloud requests.";
        return availability;
    }
    if (key)
        *key = Trimmed(*value);
    availability.bAvailable = true;
    return availability;
}

CloudAvailability CloudChatService::Availability() const
{
    return Check(Capture(), nullptr);
}

std::string CloudChatService::StatusText() const
{
    const Snapshot snapshot = Capture();
    const CloudAvailability availability = Check(snapshot, nullptr);
    std::string endpointError;
    const auto endpoint = ParseCloudEndpoint(snapshot.settings.baseUrl, endpointError);
    std::string text = availability.bAvailable ? "Cloud model is available" : "Cloud model is unavailable";
    if (endpoint)
        text += ": " + snapshot.settings.provider + ", " + snapshot.settings.model + " at " + endpoint->host;
    text += ". Used for: " + snapshot.settings.useFor + ".";
    if (!availability.bAvailable)
        text += " " + availability.reason;
    return text;
}

CloudToolSupport CloudChatService::ToolSupport() const
{
    return {false, "Tool calling is not offered through the cloud backend; tools run with the local model."};
}

responseOutput CloudChatService::GenerateResponse(const std::vector<conversationMessage>& history, const CloudTurnRequest& turn,
    const std::stop_token stopToken, DeltaHandler onDelta, ReasoningHandler onReasoning) const
{
    const Snapshot snapshot = Capture();
    if (turn.bPublicAudience)
        return Refusal("I only use the cloud model in private conversations.", "Cloud models are never used for a public audience.");

    std::string key;
    const CloudAvailability availability = Check(snapshot, &key);
    if (!availability.bAvailable)
        return Refusal("I can't use the cloud model right now. " + availability.reason, availability.reason);

    const CloudPayloadResult payload = BuildCloudPayload(snapshot.persona, history, turn);
    if (!payload.bAllowed)
        return Refusal(payload.refusal, payload.refusal);

    const CloudProvider provider = *ParseCloudProvider(snapshot.settings.provider);
    std::string endpointError;
    const CloudEndpoint endpoint = *ParseCloudEndpoint(snapshot.settings.baseUrl, endpointError);
    const int maxTokens = std::clamp(snapshot.settings.maxTokens, 1, 128000);
    const CloudWireRequest wire = BuildCloudRequest(provider, endpoint, snapshot.settings.model, maxTokens, payload.payload, key);

    audit::CloudRequestAuditEntry record;
    record.transactionId = "cloud-" + audit::ContentDigest(wire.body + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count())).substr(0, 16);
    record.provider = ToString(provider);
    record.host = endpoint.host;
    record.model = snapshot.settings.model;
    record.requestBytes = wire.body.size();
    record.requestDigest = audit::ContentDigest(wire.body);
    record.bExplicit = turn.bExplicit;
    if (!snapshot.audit(record))
    {
        return Refusal("That stayed on this PC: the cloud request could not be recorded in the action audit.",
            "The cloud request intent could not be written to the action audit, so nothing was sent.");
    }

    CloudHttpRequest request;
    request.endpoint = endpoint;
    request.path = wire.path;
    request.headers = wire.headers;
    request.body = wire.body;
    request.timeoutSeconds = std::clamp(snapshot.settings.timeoutSeconds, 1, 600);

    responseOutput output;
    output.selectedTier = "Cloud";
    output.selectedModel = "cloud:" + snapshot.settings.model;
    ServerSentEventParser parser;
    std::string text;
    std::string reasoning;
    std::string finishReason;
    std::string streamError;
    std::string errorBody;
    bool done = false;
    const auto started = std::chrono::steady_clock::now();
    std::optional<double> firstTokenMilliseconds;
    const auto apply = [&](const ServerSentEventParser::Event& event)
    {
        const CloudStreamDelta delta = DecodeCloudEvent(provider, event);
        if (!delta.error.empty())
            streamError = delta.error;
        if (!delta.reasoning.empty())
        {
            reasoning += delta.reasoning;
            if (onReasoning)
                onReasoning(delta.reasoning);
        }
        if (!delta.text.empty())
        {
            if (!firstTokenMilliseconds)
                firstTokenMilliseconds = ElapsedMilliseconds(started);
            text += delta.text;
            if (onDelta)
            {
                onDelta(delta.text);
                output.bWasStreamed = true;
            }
        }
        if (!delta.finishReason.empty())
            finishReason = delta.finishReason;
        if (delta.promptTokens)
        {
            output.promptTokens = *delta.promptTokens;
            output.bTokensReported = true;
        }
        if (delta.completionTokens)
        {
            output.completionTokens = *delta.completionTokens;
            output.bTokensReported = true;
        }
        done = done || delta.done;
    };
    const CloudHttpResult transfer = snapshot.transport->Post(request,
        [&](const int status, const std::string_view chunk)
        {
            if (status != 200)
            {
                if (errorBody.size() < 16384)
                    errorBody.append(chunk.data(), std::min<std::size_t>(chunk.size(), 16384 - errorBody.size()));
                return true;
            }
            for (const ServerSentEventParser::Event& event : parser.Feed(chunk))
                apply(event);
            return !stopToken.stop_requested();
        },
        stopToken);
    if (transfer.bReceived && transfer.status == 200)
    {
        if (const auto last = parser.Finish())
            apply(*last);
    }
    const double requestMilliseconds = ElapsedMilliseconds(started);
    output.timings.push_back({"cloud_wait_first_token", firstTokenMilliseconds.value_or(requestMilliseconds)});
    output.timings.push_back({"cloud_request_total", requestMilliseconds, true});
    output.rawResponse = text;
    output.reasoning = Trimmed(reasoning);

    record.phase = "result";
    record.status = transfer.status;
    record.responseBytes = transfer.bytesReceived;
    record.elapsedMilliseconds = requestMilliseconds;

    std::string failure;
    if (stopToken.stop_requested())
        failure = "Conversation generation was cancelled.";
    else if (!transfer.bReceived)
        failure = transfer.error.empty() ? "The cloud request failed." : transfer.error;
    else if (transfer.status != 200)
        failure = "The cloud model returned HTTP " + std::to_string(transfer.status) + ": " + ErrorPreview(errorBody);
    else if (!streamError.empty())
        failure = "The cloud model reported an error: " + revia::utf8::Prefix(streamError, 300);
    else if (finishReason == "refusal")
        failure = "The cloud model declined to answer.";

    std::string reply = Trimmed(text);
    if (failure.empty() && (finishReason == "length" || finishReason == "max_tokens"))
    {
        const std::string complete = CompleteSentences(reply);
        reply = complete.empty() ? "I ran out of room before I could finish that answer. Ask me to continue." : complete;
    }
    if (failure.empty() && reply.empty())
        failure = "The cloud model returned an empty response.";

    record.succeeded = failure.empty();
    record.outcome = failure.empty() ? "completed" : failure;
    (void)snapshot.audit(record);

    if (!failure.empty())
    {
        output.bSuccess = false;
        output.response = stopToken.stop_requested() ? "I stopped that response." : "The cloud model did not answer: " + failure;
        output.reason = failure;
        output.bShouldSpeak = true;
        return output;
    }
    output.bSuccess = true;
    output.response = reply;
    output.bShouldSpeak = true;
    output.bShouldRemember = false;
    return output;
}

} // namespace revia::llm::cloud
