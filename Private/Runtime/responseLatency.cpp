#include "Runtime/responseLatency.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>

namespace revia::runtime
{
namespace
{
LatencyDistribution Distribution(const std::deque<double>& samples)
{
    if (samples.empty())
        return {};
    std::vector<double> ordered(samples.begin(), samples.end());
    std::sort(ordered.begin(), ordered.end());
    const std::size_t middle = ordered.size() / 2;
    const double median = ordered.size() % 2 == 0 ? (ordered[middle - 1] + ordered[middle]) / 2.0 : ordered[middle];
    const auto rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(ordered.size())));
    return {ordered.size(), median, ordered[rank - 1]};
}

void AddSample(std::deque<double>& samples, const double value)
{
    samples.push_back(value);
    if (samples.size() > 64)
        samples.pop_front();
}

void Describe(std::ostringstream& text, const char* name, const double latest, const LatencyDistribution& distribution)
{
    text << name << ": ";
    if (latest < 0.0)
        text << "unmeasured";
    else
        text << latest << "ms";
    if (distribution.samples > 0)
        text << " (median " << distribution.medianMilliseconds << "ms, p95 " << distribution.p95Milliseconds << "ms; "
             << distribution.samples << " samples)";
}
}

void ResponseLatency::Reset(const RuntimeStamp& inputOrigin)
{
    std::lock_guard lock(mutex);
    origin = inputOrigin;
    turns.clear();
    utterances.clear();
    textSamples.clear();
    readySamples.clear();
    playedSamples.clear();
    latestTurn = 0;
}

bool ResponseLatency::Observe(const RuntimeEvent& event, const Clock::time_point now)
{
    std::lock_guard lock(mutex);
    if (origin.sessionId.empty() || !origin.SameSession(event.stamp))
        return false;
    if (event.component == "Response latency")
    {
        if (event.turnId == 0)
            return false;
        if (event.phase == "Started")
        {
            if (turns.contains(event.turnId))
                return false;
            const double queuedMilliseconds =
                std::isfinite(event.elapsedMilliseconds) && event.elapsedMilliseconds > 0.0 ? event.elapsedMilliseconds : 0.0;
            const auto queued = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(queuedMilliseconds));
            turns.emplace(event.turnId, Turn{now - queued});
            latestTurn = event.turnId;
            while (turns.size() > 32)
            {
                const auto retired = turns.begin()->first;
                std::erase_if(utterances, [retired](const auto& item) { return item.second == retired; });
                turns.erase(turns.begin());
            }
            return true;
        }
        const auto found = turns.find(event.turnId);
        if (found == turns.end() || now < found->second.started)
            return false;
        if (event.phase == "Cancelled")
        {
            std::erase_if(utterances, [&](const auto& item) { return item.second == event.turnId; });
            turns.erase(found);
            if (latestTurn == event.turnId)
                latestTurn = 0;
            return true;
        }
        if (event.phase == "SpeechQueued")
        {
            if (event.utteranceId == 0 || utterances.contains(event.utteranceId))
                return false;
            if (utterances.size() >= 128)
                return false;
            utterances.emplace(event.utteranceId, event.turnId);
            return true;
        }
        if (event.phase == "TextReady" && found->second.text < 0.0)
        {
            found->second.text = std::chrono::duration<double, std::milli>(now - found->second.started).count();
            AddSample(textSamples, found->second.text);
            return true;
        }
        return false;
    }
    if (event.component != "Voice")
        return false;
    const auto found = utterances.find(event.utteranceId != 0 ? event.utteranceId : event.turnId);
    if (event.phase == "Interrupted" || event.phase == "Stopped" || event.phase == "Disabled" || event.phase == "Error")
    {
        if (found == utterances.end())
        {
            if (event.turnId == 0 && event.utteranceId == 0)
                utterances.clear();
            return false;
        }
        const auto turnId = found->second;
        std::erase_if(utterances, [turnId](const auto& item) { return item.second == turnId; });
        return true;
    }
    if (found == utterances.end())
        return false;
    const auto turn = turns.find(found->second);
    if (turn == turns.end() || now < turn->second.started)
        return false;
    const double elapsed = std::chrono::duration<double, std::milli>(now - turn->second.started).count();
    if (event.phase == "FirstAudioReady" && turn->second.ready < 0.0)
    {
        turn->second.ready = elapsed;
        AddSample(readySamples, elapsed);
        return true;
    }
    if (event.phase == "FirstAudioPlayed" && turn->second.played < 0.0)
    {
        turn->second.played = elapsed;
        AddSample(playedSamples, elapsed);
        return true;
    }
    return false;
}

ResponseLatencySnapshot ResponseLatency::Snapshot() const
{
    std::lock_guard lock(mutex);
    ResponseLatencySnapshot result;
    result.origin = origin;
    result.turnId = latestTurn;
    if (const auto found = turns.find(latestTurn); found != turns.end())
    {
        result.textMilliseconds = found->second.text;
        result.firstAudioReadyMilliseconds = found->second.ready;
        result.firstAudioPlayedMilliseconds = found->second.played;
    }
    result.text = Distribution(textSamples);
    result.audioReady = Distribution(readySamples);
    result.audioPlayed = Distribution(playedSamples);
    return result;
}

std::string ResponseLatencySnapshot::Summary() const
{
    std::ostringstream text;
    text << std::fixed << std::setprecision(0);
    Describe(text, "Text", textMilliseconds, this->text);
    text << "; ";
    Describe(text, "Audio ready", firstAudioReadyMilliseconds, audioReady);
    text << "; ";
    Describe(text, "Playback", firstAudioPlayedMilliseconds, audioPlayed);
    return text.str();
}
}
