#pragma once

#include "Perception/windowEventMonitor.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace revia::perception
{

// A stretch of time spent in one application. Consecutive observations of the same
// executable extend a span rather than each becoming a separate record, which is what
// turns a stream of events into something answerable.
struct ActivitySpan
{
    std::string application;
    std::vector<std::string> titles;
    std::chrono::system_clock::time_point startedAt;
    std::chrono::system_clock::time_point endedAt;
    std::uint32_t observations = 0;
    // A window may move between displays while the application span continues. Keep a
    // small set of one-based monitor indices instead of fragmenting the activity record.
    std::vector<int> monitors;

    [[nodiscard]] std::chrono::seconds Duration() const;
};

struct ActivityHistorySettings
{
    // Bounds, not preferences. Stage 6's exit criterion is answering "what have I been
    // doing for the last hour", so the retention window only has to cover a working
    // session; keeping more would be accumulating a record nobody asked for.
    std::chrono::minutes retention{480};
    std::size_t maxSpans = 240;
    // Titles are the one part of an observation that carries document content, so they
    // are capped hard per span. Losing the fourth filename is not a real loss.
    std::size_t maxTitlesPerSpan = 4;
    // Returning to an application within this gap continues the same span instead of
    // fragmenting a working session into dozens of entries.
    std::chrono::seconds mergeGap{120};
    // Time spent in an application runs until the next one appears, not until its last
    // window event -- a quiet application generates no events, and reporting 0s for forty
    // minutes of reading would answer the question wrongly. The attribution is capped so
    // that a machine left alone overnight does not credit all of it to whatever happened
    // to be in front.
    std::chrono::seconds maxAttributedGap{300};
};

// In-memory only, and deliberately so.
//
// Structured observations may persist under the existing memory sensitivity rules, but a
// window-title history is exactly the kind of thing that should not quietly become a file
// on disk as a side effect of enabling a status chip. This forgets everything when Revia
// stops, and can be cleared on demand before that.
class ActivityHistory
{
public:
    ActivityHistory() = default;
    explicit ActivityHistory(ActivityHistorySettings settings);

    void Record(const WindowObservation& observation);
    void Clear();

    [[nodiscard]] std::vector<ActivitySpan> Spans(std::chrono::minutes window) const;
    // Human-readable rollup: what was used, for how long, most time first.
    [[nodiscard]] std::string Summarize(std::chrono::minutes window) const;
    [[nodiscard]] std::size_t Size() const;

private:
    // Callers already hold the mutex.
    void PruneLocked(std::chrono::system_clock::time_point now);

    mutable std::mutex mutex;
    ActivityHistorySettings configuration;
    std::vector<ActivitySpan> spans;
};

} // namespace revia::perception
