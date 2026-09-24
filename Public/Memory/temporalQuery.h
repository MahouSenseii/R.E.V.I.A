#pragma once

#include <cstdint>
#include <string>

namespace revia::memory
{

// A half-open span of epoch seconds named by something the user said.
//
// Produced deterministically from the utterance, never by a model. "Yesterday" is a
// fact about the clock, and asking a language model to resolve it would make recall
// depend on whether the model happened to know what day it is.
struct TimeWindow
{
    // Inclusive lower bound, exclusive upper bound, both epoch seconds.
    std::int64_t startEpoch = 0;
    std::int64_t endEpoch = 0;
    // The phrase that produced the window, kept so a caller can say which part of the
    // question it answered rather than asserting a span the user never named.
    std::string phrase;

    [[nodiscard]] bool IsValid() const
    {
        return endEpoch > startEpoch;
    }

    [[nodiscard]] bool Contains(std::int64_t epochSeconds) const
    {
        return epochSeconds >= startEpoch && epochSeconds < endEpoch;
    }
};

// Resolves the first time reference in `text` against `nowEpoch`, in local time.
//
// Returns an invalid window when the text names no time, which is the common case and
// is not an error. Recognises the phrasings people actually use to reach back into a
// conversation -- "yesterday", "last night", "three days ago", "last Tuesday",
// "2026-08-29" -- and deliberately stops there. An unrecognised phrase costs nothing
// beyond the retrieval behaviour that existed before this parser.
[[nodiscard]] TimeWindow ParseTimeWindow(const std::string& text, std::int64_t nowEpoch);

// Renders a moment the way someone would say it out loud: "yesterday 19:42" rather than
// "1756500000". Relative near the present, absolute once relative stops being useful.
[[nodiscard]] std::string DescribeMoment(std::int64_t epochSeconds, std::int64_t nowEpoch);

// "Tuesday 1 September 2026, 14:32" -- the anchor that makes every relative phrase above
// readable. Without it a prompt full of "yesterday" says nothing.
[[nodiscard]] std::string DescribeNow(std::int64_t nowEpoch);

// Epoch seconds for this instant, shared so callers do not each reimplement it.
[[nodiscard]] std::int64_t CurrentEpoch();

// Both stores hold created_at as epoch seconds in a TEXT column, and neither guarantees
// the value is a number -- legacy JSONL imports allow anything. Returns 0 for anything
// that is not a plain count of seconds, which every caller treats as "no timestamp"
// rather than as a moment in 1970.
[[nodiscard]] std::int64_t ParseEpochSecondsText(const std::string& value);

} // namespace revia::memory
