#pragma once

#include "Library/structLibrary.h"

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

// Decides what is worth answering, and what several arrivals actually amount to.
//
// A recogniser that is always listening produces a stream, not a question. Most of it is
// room noise, filler, and the same phrase heard twice; some of it is one thought split
// across three pauses. Answering each arrival separately is what makes an always-on
// assistant exhausting, so inputs are filtered on the way in and merged on the way out.
//
// Typed input is never filtered. Someone who took the trouble to type "ok" meant it.
class InputArbiter
{
public:
    InputArbiter() = default;
    explicit InputArbiter(inputArbiterSettings settings);

    [[nodiscard]] InputVerdict Offer(
        const std::string& text,
        InputSource source,
        std::chrono::system_clock::time_point now);

    // True once the merge window has closed on what is queued.
    [[nodiscard]] bool IsReady(std::chrono::system_clock::time_point now) const;
    // Everything queued, joined into one turn, and the queue emptied.
    [[nodiscard]] std::string Take();
    [[nodiscard]] std::size_t Size() const;
    void Clear();

    [[nodiscard]] static bool IsNoise(
        const inputArbiterSettings& settings,
        const std::string& text);
    [[nodiscard]] static std::string Normalize(const std::string& text);

private:
    mutable std::mutex mutex;
    inputArbiterSettings configuration;
    std::vector<PendingInput> queued;
    std::string lastAccepted;
    std::chrono::system_clock::time_point lastAcceptedAt{};
};

} // namespace revia::agents
