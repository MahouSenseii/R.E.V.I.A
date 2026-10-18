#pragma once

#include "Audit/cloudRequestAudit.h"
#include "Core/conversationMessage.h"
#include "LLM/Cloud/cloudPolicy.h"
#include "LLM/Cloud/cloudSettings.h"
#include "LLM/Cloud/cloudTransport.h"
#include "LLM/responseTypes.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::llm::cloud
{

struct CloudAvailability
{
    bool bAvailable = false;
    // Why not, in words fit for the chat and the log. Never contains the key.
    std::string reason;
};

// Whether a cloud provider can be handed tool definitions. Nothing here offers tools yet;
// a provider that does overrides ToolSupport and reports what it accepts.
struct CloudToolSupport
{
    bool bSupported = false;
    std::string reason;
};

// The optional cloud chat backend: an OpenAI-compatible chat-completions endpoint or the
// Anthropic Messages API, streamed, behind the same responseOutput the local model fills.
//
// Every request passes the same gates, here rather than in a caller that could forget one:
// enabled in settings, not switched off with /cloud off, a key in the named environment
// variable, never a public audience, a payload the sensitive-content detector does not
// object to, and an audit record written before any byte leaves.
class CloudChatService
{
public:
    using DeltaHandler = std::function<void(const std::string&)>;
    using ReasoningHandler = std::function<void(const std::string&)>;
    // Reads one environment variable. Replaceable so tests never depend on the real one.
    using EnvironmentReader = std::function<std::optional<std::string>(const std::string&)>;
    using AuditSink = std::function<bool(const audit::CloudRequestAuditEntry&)>;

    CloudChatService();
    virtual ~CloudChatService();

    void ApplySettings(const CloudModelSettings& settings);
    void SetPersona(std::string systemPrompt);
    void SetTransport(std::shared_ptr<CloudTransport> transport);
    void SetEnvironmentReader(EnvironmentReader reader);
    // Without a sink nothing is sent: an unaudited cloud request is not allowed to exist.
    void SetAuditSink(AuditSink sink);
    // /cloud on|off. On by default, so llm.cloud.enabled alone decides until the user
    // switches it off for the session.
    void SetSessionEnabled(bool enabled);
    [[nodiscard]] bool SessionEnabled() const;

    [[nodiscard]] CloudModelSettings Settings() const;
    [[nodiscard]] CloudUse Use() const;
    [[nodiscard]] CloudAvailability Availability() const;
    // One line for /cloud: on or off, which provider, host and model, and why not.
    [[nodiscard]] std::string StatusText() const;

    [[nodiscard]] responseOutput GenerateResponse(const std::vector<conversationMessage>& history, const CloudTurnRequest& turn,
        std::stop_token stopToken = {}, DeltaHandler onDelta = {}, ReasoningHandler onReasoning = {}) const;

    [[nodiscard]] virtual CloudToolSupport ToolSupport() const;

private:
    struct Snapshot
    {
        CloudModelSettings settings;
        std::string persona;
        std::shared_ptr<CloudTransport> transport;
        EnvironmentReader environment;
        AuditSink audit;
        bool sessionEnabled = true;
    };
    [[nodiscard]] Snapshot Capture() const;
    // Checks everything but the key's presence when `key` is null; with it, also reads it.
    [[nodiscard]] static CloudAvailability Check(const Snapshot& snapshot, std::string* key);

    mutable std::mutex mutex;
    Snapshot state;
};

} // namespace revia::llm::cloud
