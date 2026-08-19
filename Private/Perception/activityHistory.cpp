#include "Perception/activityHistory.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace revia::perception
{

namespace
{

std::string FormatDuration(const std::chrono::seconds value)
{
    const std::int64_t total = value.count();
    if (total < 60)
    {
        return std::to_string(total) + "s";
    }
    const std::int64_t minutes = total / 60;
    if (minutes < 60)
    {
        return std::to_string(minutes) + "m";
    }
    return std::to_string(minutes / 60) + "h" + std::to_string(minutes % 60) + "m";
}

} // namespace

std::chrono::seconds ActivitySpan::Duration() const
{
    if (endedAt <= startedAt)
    {
        return std::chrono::seconds{0};
    }
    return std::chrono::duration_cast<std::chrono::seconds>(endedAt - startedAt);
}

ActivityHistory::ActivityHistory(ActivityHistorySettings settings)
    : configuration(std::move(settings))
{
}

void ActivityHistory::Record(const WindowObservation& observation)
{
    if (observation.application.empty())
    {
        return;
    }
    std::lock_guard lock(mutex);

    const bool continues = !spans.empty() &&
        spans.back().application == observation.application &&
        observation.occurredAt >= spans.back().endedAt &&
        observation.occurredAt - spans.back().endedAt <= configuration.mergeGap;

    if (continues)
    {
        ActivitySpan& current = spans.back();
        current.endedAt = observation.occurredAt;
        ++current.observations;
        if (!observation.windowTitle.empty() &&
            current.titles.size() < configuration.maxTitlesPerSpan &&
            std::find(current.titles.begin(), current.titles.end(), observation.windowTitle) ==
                current.titles.end())
        {
            current.titles.push_back(observation.windowTitle);
        }
    }
    else
    {
        // Close the previous span at the moment this one appeared. Without this a span
        // ends at its own last window event, so an application used quietly reports no
        // time at all.
        if (!spans.empty() && observation.occurredAt > spans.back().endedAt)
        {
            const auto gap = observation.occurredAt - spans.back().endedAt;
            spans.back().endedAt += std::min(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(gap),
                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    configuration.maxAttributedGap));
        }

        ActivitySpan opened;
        opened.application = observation.application;
        opened.startedAt = observation.occurredAt;
        opened.endedAt = observation.occurredAt;
        opened.observations = 1;
        if (!observation.windowTitle.empty())
        {
            opened.titles.push_back(observation.windowTitle);
        }
        spans.push_back(std::move(opened));
    }

    PruneLocked(observation.occurredAt);
}

void ActivityHistory::PruneLocked(const std::chrono::system_clock::time_point now)
{
    const auto cutoff = now - configuration.retention;
    const auto expired = std::find_if(
        spans.begin(),
        spans.end(),
        [cutoff](const ActivitySpan& span) { return span.endedAt >= cutoff; });
    if (expired != spans.begin())
    {
        spans.erase(spans.begin(), expired);
    }
    if (spans.size() > configuration.maxSpans)
    {
        spans.erase(
            spans.begin(),
            spans.begin() + static_cast<std::ptrdiff_t>(spans.size() - configuration.maxSpans));
    }
}

void ActivityHistory::Clear()
{
    std::lock_guard lock(mutex);
    spans.clear();
}

std::vector<ActivitySpan> ActivityHistory::Spans(const std::chrono::minutes window) const
{
    std::lock_guard lock(mutex);
    const auto cutoff = std::chrono::system_clock::now() - window;
    std::vector<ActivitySpan> selected;
    for (const ActivitySpan& span : spans)
    {
        if (span.endedAt >= cutoff)
        {
            selected.push_back(span);
        }
    }
    return selected;
}

std::string ActivityHistory::Summarize(const std::chrono::minutes window) const
{
    const std::vector<ActivitySpan> selected = Spans(window);
    if (selected.empty())
    {
        return "Nothing observed in that period.";
    }

    // Totalled per application rather than listed per span: "three hours in the editor"
    // is the answer to the question, and a list of forty switches is not.
    struct Total
    {
        std::string application;
        std::chrono::seconds duration{0};
        std::uint32_t visits = 0;
        std::vector<std::string> titles;
    };
    std::vector<Total> totals;
    for (const ActivitySpan& span : selected)
    {
        const auto existing = std::find_if(
            totals.begin(),
            totals.end(),
            [&span](const Total& total) { return total.application == span.application; });
        Total& total = existing != totals.end()
            ? *existing
            : totals.emplace_back(Total{span.application, {}, 0, {}});
        total.duration += span.Duration();
        ++total.visits;
        for (const std::string& title : span.titles)
        {
            if (total.titles.size() < 4 &&
                std::find(total.titles.begin(), total.titles.end(), title) == total.titles.end())
            {
                total.titles.push_back(title);
            }
        }
    }

    std::sort(totals.begin(), totals.end(), [](const Total& left, const Total& right)
    {
        if (left.duration != right.duration)
        {
            return left.duration > right.duration;
        }
        return left.visits > right.visits;
    });

    std::ostringstream stream;
    stream << "In the last " << window.count() << " minutes:";
    for (const Total& total : totals)
    {
        stream << "\n  " << FormatDuration(total.duration) << "  " << total.application;
        if (total.visits > 1)
        {
            stream << " (" << total.visits << " visits)";
        }
        if (!total.titles.empty())
        {
            stream << "\n      ";
            for (std::size_t index = 0; index < total.titles.size(); ++index)
            {
                stream << (index == 0 ? "" : ", ") << total.titles[index];
            }
        }
    }
    return stream.str();
}

std::size_t ActivityHistory::Size() const
{
    std::lock_guard lock(mutex);
    return spans.size();
}

} // namespace revia::perception
