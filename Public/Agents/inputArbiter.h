#pragma once


#include "Agents/inputArbiterSettings.h"
#include <chrono>
#include <mutex>
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

struct PendingInput
{
    std::string text;
    InputSource source = InputSource::Typed;
    std::chrono::system_clock::time_point receivedAt;
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

    // True once the merge window has closed on what is queued.
    [[nodiscard]] bool IsReady(std::chrono::system_clock::time_point now) const;
    // Everything queued, joined into one turn, and the queue emptied.
    [[nodiscard]] std::string Take();
    [[nodiscard]] std::size_t Size() const;
    void Clear();

    [[nodiscard]] static bool IsNoise(const inputArbiterSettings& settings, const std::string& text);
    [[nodiscard]] static std::string Normalize(const std::string& text);

private:
    mutable std::mutex mutex;
    inputArbiterSettings configuration;
    std::vector<PendingInput> queued;
    std::string lastAccepted;
    std::chrono::system_clock::time_point lastAcceptedAt{};
};

} // namespace revia::agents
