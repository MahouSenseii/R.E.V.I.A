#pragma once

#include "Core/profile.h"
#include "LLM/endpointSettings.h"
#include "Runtime/sessionResult.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace revia::presence
{

// A loopback OpenAI-compatible endpoint, so other programs on this PC can talk to Revia:
// GET /v1/models and POST /v1/chat/completions, streamed or not. Every request needs the
// install's bearer token; a browser Origin, a chunked or oversized body, or a Host other
// than this loopback address is refused before the body is read.
//
// By default a request is a shared audience, isolated the way the web guest is: its own
// router and profile, only the conversation the request carries, no private memory,
// screen, clipboard, tools, or identity and relationship writes. The owner can instead
// make each request an owner turn by supplying `privateTurn` (settings: trustedPrivate).
class LocalApiRuntime
{
  public:
    // Runs one owner turn and returns the delivered reply. Only set when the owner
    // explicitly trusts the API with the private audience.
    using PrivateTurn = std::function<runtime::SessionResult(const std::string& input, std::stop_token stopToken)>;

    // `ownerBusy` is true while the owner's own turn runs; shared requests yield to it.
    LocalApiRuntime(const llmSettings& model, std::function<bool()> ownerBusy);
    ~LocalApiRuntime();
    LocalApiRuntime(const LocalApiRuntime&) = delete;
    LocalApiRuntime& operator=(const LocalApiRuntime&) = delete;

    // Binds 127.0.0.1:`port` (0 picks a free port). False when the token is malformed,
    // the model is not on this PC, or the port is taken.
    [[nodiscard]] bool Start(int port, const std::string& token, PrivateTurn privateTurn = {});
    void Stop();
    // Cancels a shared request in flight, because the owner needs Revia.
    void Preempt();
    [[nodiscard]] int Port() const;
    [[nodiscard]] bool Trusted() const;

    // The fixed persona a shared request is answered with. Never the owner's profile.
    [[nodiscard]] static aiProfile SharedProfile();
    // The model id /v1/models lists.
    static constexpr const char* ModelId = "revia";
    static constexpr std::size_t MaximumBodyBytes = 65536;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace revia::presence
