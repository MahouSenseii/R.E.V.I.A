#pragma once

#include <chrono>
#include <string>

namespace revia::speech
{

// Whether what someone just said was the end of their thought.
//
// Silence alone decides that badly. A person who pauses after "and then I" has not
// finished, and a person who says "okay." has, however long or short the gap after it.
// Smart Turn and its kind judge this with a model over the audio; until one is in the
// build, the words themselves carry most of the signal, and this is that judgement
// made deterministic: a trailing conjunction, filler or comma means wait, a terminal
// mark or a whole short phrase means go, and everything else keeps the configured gap.
enum class TurnCompletion
{
    Complete,
    Unfinished,
    Uncertain
};

struct TurnJudgement
{
    TurnCompletion completion = TurnCompletion::Uncertain;
    // The token or mark that decided it, for the log.
    std::string because;
};

[[nodiscard]] std::string ToString(TurnCompletion value);
[[nodiscard]] TurnJudgement JudgeTurnCompletion(const std::string& transcript);

// How long to wait for the rest of a thought before answering, given the judgement:
// the configured gap for a complete one, the gap plus the extension for one that
// trailed off, and half the extension when it is unclear.
[[nodiscard]] int ContinuationWindowMs(TurnCompletion completion, int baseSilenceMs, int extensionMs);

// The sound she makes while she stops to think, so a long pause before a hard answer
// reads as thinking rather than as a dropped connection.
//
// Rare by design: once per turn, never twice within the interval, and only when the
// turn is a real question she is about to think about. The clip is the voice bank's
// own "hmm", so it is her sound; nothing is synthesized for it.
class ThinkingFillerPolicy
{
public:
    using Clock = std::chrono::steady_clock;

    explicit ThinkingFillerPolicy(std::chrono::seconds minimumInterval = std::chrono::seconds(45))
        : minimumInterval(minimumInterval) {}

    // True when a filler should play now for this turn. Records the decision.
    [[nodiscard]] bool Consider(
        const std::string& input,
        bool willThink,
        bool speechAvailable,
        std::uint64_t turnId,
        Clock::time_point now);

    // Whether the words ask something worth a visible pause: a question, or a request
    // to explain or work something out.
    [[nodiscard]] static bool AsksForThought(const std::string& input);

private:
    std::chrono::seconds minimumInterval;
    std::uint64_t lastTurn = 0;
    Clock::time_point lastFiller{};
};

} // namespace revia::speech
