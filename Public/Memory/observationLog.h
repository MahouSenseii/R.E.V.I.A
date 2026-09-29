#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace revia::memory
{

// One thing worth keeping from conversation that has left the window.
//
// Written by a model from the turns being folded, in the third person, with who said
// it. Dated twice: when it was observed, and -- when the words said so -- the moment it
// refers to. A note that refers to "tomorrow" is useless a week later unless it also says
// when tomorrow was.
struct Observation
{
    std::uint64_t id = 0;
    std::string text;
    // 1 minor, 2 ordinary, 3 something later turns will depend on.
    int priority = 2;
    // fact, preference, decision, promise, question, topic or event.
    std::string kind;
    // Epoch seconds.
    std::int64_t observedAt = 0;
    // The moment the observation refers to, in the speaker's own words, or empty.
    std::string refersTo;
    // The conversation sequence numbers it was drawn from. Zero for an observation
    // carried over from an earlier session.
    std::uint64_t sourceFrom = 0;
    std::uint64_t sourceTo = 0;
    // The observation that replaced this one, or zero while it is current.
    std::uint64_t supersededBy = 0;

    [[nodiscard]] bool Current() const { return supersededBy == 0; }
};

// The record of a conversation's earlier part: dated observations, appended as turns
// leave the window and merged, never rewritten, as the log grows.
//
// This replaced one running summary. A summary the model rewrites every time it grows
// loses detail with each pass, because a small model summarising its own summary keeps
// less each time (the "context collapse" the ACE paper measured). Observations are
// only ever added, or superseded by a merged observation that names what it replaced,
// so every line in the prompt traces to the turns it came from and a forget can remove
// everything derived from them.
//
// Not thread-safe on its own; conversationContext holds it under its mutex.
class ObservationLog
{
public:
    // Appends and assigns the id. An empty text is ignored and returns zero.
    std::uint64_t Append(Observation observation);
    // Records `merged` as replacing every id in `replaced`. Refused, returning zero,
    // unless every id names a current observation and there are at least two of them:
    // a merge of one is a rewrite.
    std::uint64_t Supersede(const std::vector<std::uint64_t>& replaced, Observation merged);
    // Replaces the whole log, ids included. For restoring a session.
    void Restore(std::vector<Observation> restored);
    void Clear();

    // Current observations, oldest first.
    [[nodiscard]] std::vector<Observation> Current() const;
    // Everything, including superseded, oldest first. For persistence and the panel.
    [[nodiscard]] const std::vector<Observation>& All() const { return observations; }
    [[nodiscard]] bool Empty() const;
    [[nodiscard]] std::size_t CurrentCount() const;

    // The current observations as prompt text, oldest first, inside `budget` bytes.
    //
    // When the log does not fit, the oldest minor observations go first, then the
    // oldest ordinary ones; important ones stay as long as anything else can go. The
    // dates are rendered relative to `nowEpoch` so the model never does arithmetic.
    [[nodiscard]] std::string Render(std::size_t budget, std::int64_t nowEpoch) const;

    // Whether the log has grown past the point where merging related observations is
    // worth a model call.
    [[nodiscard]] bool NeedsReflection() const;

    // How much of the prompt the log may take.
    // One-sentence observations run 80-120 bytes, so this holds thirty-odd of them:
    // the record normally fits whole until reflection merges it back down.
    static constexpr std::size_t RenderBudget = 3600;
    // Reflection starts above either of these.
    static constexpr std::size_t ReflectAboveObservations = 36;
    static constexpr std::size_t ReflectAboveCharacters = 4800;
    static constexpr std::size_t MaximumObservationCharacters = 240;

private:
    std::vector<Observation> observations;
    std::uint64_t nextId = 1;
};

} // namespace revia::memory
