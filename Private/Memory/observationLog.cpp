#include "Memory/observationLog.h"

#include "Core/utf8.h"
#include "Memory/temporalQuery.h"

#include <algorithm>
#include <set>
#include <utility>

namespace revia::memory
{

namespace
{
std::string Bounded(std::string text)
{
    for (char& character : text)
    {
        if (character == '\r' || character == '\n') character = ' ';
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    if (text.size() > ObservationLog::MaximumObservationCharacters)
    {
        revia::utf8::Truncate(text, ObservationLog::MaximumObservationCharacters);
    }
    return text;
}

std::string Line(const Observation& observation, const std::int64_t nowEpoch)
{
    std::string line = "- ";
    if (const std::string when = DescribeMoment(observation.observedAt, nowEpoch);
        !when.empty())
    {
        line += "(" + when;
        if (!observation.refersTo.empty())
        {
            line += ", about " + observation.refersTo;
        }
        line += ") ";
    }
    else if (!observation.refersTo.empty())
    {
        line += "(about " + observation.refersTo + ") ";
    }
    line += observation.text;
    line += '\n';
    return line;
}
}

std::uint64_t ObservationLog::Append(Observation observation)
{
    observation.text = Bounded(std::move(observation.text));
    if (observation.text.empty() || !revia::utf8::IsValid(observation.text)) return 0;
    observation.priority = std::clamp(observation.priority, 1, 3);
    observation.supersededBy = 0;
    observation.id = nextId++;
    observations.push_back(std::move(observation));
    return observations.back().id;
}

std::uint64_t ObservationLog::Supersede(
    const std::vector<std::uint64_t>& replaced,
    Observation merged)
{
    const std::set<std::uint64_t> wanted(replaced.begin(), replaced.end());
    if (wanted.size() < 2) return 0;
    std::vector<Observation*> targets;
    for (Observation& observation : observations)
    {
        if (wanted.contains(observation.id) && observation.Current())
        {
            targets.push_back(&observation);
        }
    }
    if (targets.size() != wanted.size()) return 0;

    // The merged observation spans what it replaced, and is dated by the newest of
    // them: it is a record of those moments, not of the merge.
    merged.sourceFrom = 0;
    merged.sourceTo = 0;
    merged.observedAt = 0;
    for (const Observation* target : targets)
    {
        if (merged.sourceFrom == 0 || (target->sourceFrom != 0 &&
                target->sourceFrom < merged.sourceFrom))
        {
            merged.sourceFrom = target->sourceFrom;
        }
        merged.sourceTo = std::max(merged.sourceTo, target->sourceTo);
        merged.observedAt = std::max(merged.observedAt, target->observedAt);
        merged.priority = std::max(merged.priority, target->priority);
    }
    const std::uint64_t id = Append(std::move(merged));
    if (id == 0) return 0;
    for (Observation* target : targets)
    {
        target->supersededBy = id;
    }
    return id;
}

void ObservationLog::Restore(std::vector<Observation> restored)
{
    observations = std::move(restored);
    nextId = 1;
    for (const Observation& observation : observations)
    {
        nextId = std::max(nextId, observation.id + 1);
    }
}

void ObservationLog::Clear()
{
    observations.clear();
    // Ids keep counting up, so a job taken before the clear can never name one of the
    // observations added after it.
}

std::vector<Observation> ObservationLog::Current() const
{
    std::vector<Observation> current;
    for (const Observation& observation : observations)
    {
        if (observation.Current()) current.push_back(observation);
    }
    return current;
}

bool ObservationLog::Empty() const
{
    return std::none_of(observations.begin(), observations.end(),
        [](const Observation& observation) { return observation.Current(); });
}

std::size_t ObservationLog::CurrentCount() const
{
    return static_cast<std::size_t>(std::count_if(observations.begin(), observations.end(),
        [](const Observation& observation) { return observation.Current(); }));
}

std::string ObservationLog::Render(const std::size_t budget, const std::int64_t nowEpoch) const
{
    std::vector<const Observation*> kept;
    std::size_t total = 0;
    for (const Observation& observation : observations)
    {
        if (!observation.Current()) continue;
        kept.push_back(&observation);
        total += Line(observation, nowEpoch).size();
    }
    // Drop from the least important end until it fits: the oldest minor line first,
    // then the oldest ordinary one, and the important ones only when nothing else is
    // left to give.
    for (int priority = 1; priority <= 3 && total > budget; ++priority)
    {
        for (auto it = kept.begin(); it != kept.end() && total > budget;)
        {
            if ((*it)->priority == priority)
            {
                total -= Line(**it, nowEpoch).size();
                it = kept.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    std::string rendered;
    for (const Observation* observation : kept)
    {
        rendered += Line(*observation, nowEpoch);
    }
    return rendered;
}

bool ObservationLog::NeedsReflection() const
{
    std::size_t count = 0;
    std::size_t characters = 0;
    for (const Observation& observation : observations)
    {
        if (!observation.Current()) continue;
        ++count;
        characters += observation.text.size();
    }
    return count > ReflectAboveObservations || characters > ReflectAboveCharacters;
}

} // namespace revia::memory
