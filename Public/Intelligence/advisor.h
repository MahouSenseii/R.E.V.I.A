#pragma once

#include "Library/structLibrary.h"
#include "LLM/httpsTransport.h"

#include <cstddef>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::intelligence
{

// A frontier model consulted, never seated.
//
// Revia's own reply is always written by the local brains; on a hard turn she may first
// send a brief to a remote model and read its notes -- facts, a plan, verbatim code,
// what it could not settle -- as input to check. Three things decide whether a turn
// consults: deterministic rules (never the small model's opinion of its own
// difficulty, which Anthropic's own measurements say a weak executor loses), a brief
// that only ever carries what the share level allows and has been checked for a
// credential, and a budget. Every consult is visible in the Activity feed with what
// left the machine.

// What a turn sends. Rendered as one user message; the question is the person's own
// words, the context the last few turns when the share level allows, the facts a few
// lines from the runtime (the date, what she is).
struct AdvisorBrief
{
    std::string question;
    std::string context;
    std::string facts;
    // What was withheld and why, in words that never contain what was withheld.
    std::vector<std::string> redactions;
    // Set when the brief must not be sent at all: the question itself carries a
    // credential, or nothing is left of it.
    bool refused = false;
    std::string refusal;

    [[nodiscard]] std::string Render() const;
    [[nodiscard]] std::size_t Characters() const;
};

// Why a turn consults, or does not.
enum class AdvisorTrigger
{
    None,
    // The person asked for it in the turn ("ask Claude", "check with the advisor").
    Explicit,
    // /consult, or a caller that decided for its own reason.
    Forced,
    Code,
    MultiStep,
    // A fact newer than the local model can know: a recent year, "latest", "this week".
    Recency,
    Uncertainty,
    // A long input: a document to analyse.
    Length
};

struct AdvisorDecision
{
    bool consult = false;
    AdvisorTrigger trigger = AdvisorTrigger::None;
    std::string reason;
};

[[nodiscard]] std::string ToString(AdvisorTrigger trigger);

// Whether the words ask for the advisor: "ask claude", "ask the advisor", "consult the
// cloud", "check with the big brain", "take this to the advisor".
[[nodiscard]] bool AsksForAdvisor(const std::string& input);

// The deterministic decision. `escalation` is advisorSettings::escalation: "never",
// "ask" (explicit or forced only) or "auto" (the rules too). A public audience never
// consults: a chat line is not the owner's money to spend.
[[nodiscard]] AdvisorDecision DecideAdvisorConsult(
    const std::string& input,
    const std::string& escalation,
    bool previousUncertainty,
    bool publicAudience,
    bool forced);

// The brief, redacted. `recentTurns` are the turns before this one, oldest first, in
// conversationMessage form ("user"/"assistant"); they enter only when `share` is
// "conversation", newest first until `maximumCharacters` is spent, each cut to a
// bounded length, and one that carries a credential is withheld and said so. A
// question that carries one refuses the whole brief. C:\Users\<name>\ becomes
// C:\Users\<user>\ everywhere, so a pasted path does not carry the account name.
[[nodiscard]] AdvisorBrief BuildAdvisorBrief(
    const std::string& question,
    const std::vector<conversationMessage>& recentTurns,
    const std::string& runtimeFacts,
    const std::string& share,
    std::size_t maximumCharacters);

struct AdvisorNotes
{
    bool succeeded = false;
    std::string notes;
    std::string model;
    int inputTokens = 0;
    int outputTokens = 0;
    int status = 0;
    double elapsedMilliseconds = -1.0;
    std::string reason;
};

class AdvisorClient
{
public:
    explicit AdvisorClient(std::unique_ptr<llm::HttpsTransport> transport = llm::MakeSystemHttpsTransport());

    // The key is held here and nowhere else. Empty means no key: Consult refuses
    // without touching the transport.
    void Configure(advisorSettings settings, std::string apiKey);
    // New settings, same key: what a session switch (/advisor auto) changes.
    void ApplySettings(advisorSettings settings);
    // Replaces the transport and keeps the settings and the key: how a test puts a
    // fake behind a client the session already configured.
    void SetTransport(std::unique_ptr<llm::HttpsTransport> replacement);
    [[nodiscard]] bool HasKey() const { return !apiKey.empty(); }
    [[nodiscard]] const advisorSettings& Settings() const { return configuration; }
    [[nodiscard]] std::string TransportDescription() const { return transport->Describe(); }
    [[nodiscard]] bool TransportAvailable() const { return transport->Available(); }

    [[nodiscard]] AdvisorNotes Consult(const AdvisorBrief& brief, std::stop_token stopToken) const;

    // The request as it would be sent, key included: for the tests, which use a fake.
    [[nodiscard]] llm::HttpsRequest BuildRequest(const AdvisorBrief& brief) const;
    [[nodiscard]] static AdvisorNotes ParseResponse(
        const advisorSettings& settings, const llm::HttpsResponse& response, double elapsedMilliseconds);
    // What the advisor is told it is. Notes, not a reply: no address, no persona.
    [[nodiscard]] static std::string SystemPrompt();
    // The block her own prompt carries: the notes framed as input to check.
    [[nodiscard]] static std::string RenderNotesForPrompt(const AdvisorNotes& notes);
    // The endpoint path the dialect uses when settings name none.
    [[nodiscard]] static std::string DefaultPath(const std::string& dialect);

private:
    std::unique_ptr<llm::HttpsTransport> transport;
    advisorSettings configuration;
    std::string apiKey;
};

} // namespace revia::intelligence
