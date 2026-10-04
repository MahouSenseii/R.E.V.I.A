#pragma once

#include "Runtime/runtimeEvents.h"

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace revia::runtime
{

struct LatencyDistribution
{
    std::size_t samples = 0;
    double medianMilliseconds = -1.0;
    double p95Milliseconds = -1.0;
};

struct ResponseLatencySnapshot
{
    RuntimeStamp origin;
    std::uint64_t turnId = 0;
    double textMilliseconds = -1.0;
    double firstAudioReadyMilliseconds = -1.0;
    double firstAudioPlayedMilliseconds = -1.0;
    LatencyDistribution text;
    LatencyDistribution audioReady;
    LatencyDistribution audioPlayed;

    [[nodiscard]] std::string Summary() const;
};

class ResponseLatency
{
  public:
    using Clock = std::chrono::steady_clock;
    void Reset(const RuntimeStamp& origin);
    bool Observe(const RuntimeEvent& event, Clock::time_point now = Clock::now());
    [[nodiscard]] ResponseLatencySnapshot Snapshot() const;

  private:
    struct Turn
    {
        Clock::time_point started;
        double text = -1.0;
        double ready = -1.0;
        double played = -1.0;
    };
    mutable std::mutex mutex;
    RuntimeStamp origin;
    std::map<std::uint64_t, Turn> turns;
    std::map<std::uint64_t, std::uint64_t> utterances;
    std::deque<double> textSamples;
    std::deque<double> readySamples;
    std::deque<double> playedSamples;
    std::uint64_t latestTurn = 0;
};

}
