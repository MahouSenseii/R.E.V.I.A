#pragma once


#include "Agents/inputArbiterSettings.h"
#include "Identity/socialIdentity.h"
#include "Runtime/runtimeStamp.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::agents
{

enum class InputSource
{
    Typed,
    Voice,
    Proposal
};

struct InputContext
{
    runtime::RuntimeStamp stamp;
    identity::AudienceContext audience;
    std::string participantId;
    identity::SpeakerSource participantSource = identity::SpeakerSource::Unknown;
    std::uint64_t consentRevision = 0;
};

struct InputBatch
{
    std::string text;
    InputContext context;
    InputSource source = InputSource::Typed;
    bool contextMatched = true;
    std::chrono::steady_clock::time_point acceptedAt{};
};

struct PendingInput
{
    std::string text;
    InputSource source = InputSource::Typed;
    std::chrono::system_clock::time_point receivedAt;
    InputContext context;
    std::chrono::steady_clock::time_point acceptedAt{};
    // The speaker marked the end of the utterance (push-to-talk), so nothing more of
    // this thought is coming and there is no merge window to wait out.
    bool complete = false;
};

enum class InputVerdict
{
    Queued,
    IgnoredEmpty,
    IgnoredNoise,
    IgnoredDuplicate,
    DroppedOverflow
};

[[nodiscard]] std::string ToString(InputVerdict value);

// Filters voice noise and duplicates, then merges fragments into admitted input.
// Typed input is never filtered.
class InputArbiter
{
public:
    InputArbiter() = default;
    explicit InputArbiter(inputArbiterSettings settings);

    void Configure(inputArbiterSettings settings);

    [[nodiscard]] InputVerdict Offer(const std::string& text, InputSource source, std::chrono::system_clock::time_point now);
    [[nodiscard]] InputVerdict Offer(const std::string& text, InputSource source, std::chrono::system_clock::time_point now,
        InputContext context, bool complete = false);

    // True once the merge window has closed on what is queued.
    [[nodiscard]] bool IsReady(std::chrono::system_clock::time_point now) const;
    // When what is queued becomes ready, or nothing while the queue is empty or holds
    // typed input (which its own Submit call takes).
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> ReadyAt() const;
    // Blocks until queued input is ready, the stop token fires, or `limit` passes, and
    // says whether input is ready. Wakes when input arrives and again when its merge
    // window closes, so a voice turn starts as soon as it may rather than on a poll.
    bool WaitUntilReady(std::stop_token stopToken, std::chrono::milliseconds limit);
    // Everything queued, joined into one turn, and the queue emptied.
    [[nodiscard]] std::string Take();
    [[nodiscard]] InputBatch TakeBatch();
    [[nodiscard]] std::size_t Size() const;
    void Clear();

    [[nodiscard]] static bool IsNoise(const inputArbiterSettings& settings, const std::string& text);
    [[nodiscard]] static std::string Normalize(const std::string& text);
    // How long the merge window stays open after this fragment.
    [[nodiscard]] static std::chrono::milliseconds MergeWindowFor(const inputArbiterSettings& settings, const PendingInput& input);

private:
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> ReadyAtLocked() const;

    mutable std::mutex mutex;
    std::condition_variable_any changed;
    std::uint64_t revision = 0;
    inputArbiterSettings configuration;
    std::vector<PendingInput> queued;
    std::string lastAccepted;
    std::chrono::system_clock::time_point lastAcceptedAt{};
    InputContext lastAcceptedContext;
};

} // namespace revia::agents
