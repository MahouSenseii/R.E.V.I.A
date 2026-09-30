#pragma once


#include "Speech/bargeInSettings.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace revia::speech
{

// Listens for the user starting to talk while Revia is talking, and yields.
//
// Runs only during playback. A microphone that is open whenever Revia is running is a
// different and much larger promise than one that opens for the seconds it is speaking,
// and this is the smaller one: it starts when speech starts, stops when speech stops, and
// never retains a sample. Energy only -- nothing is transcribed and nothing is stored.
class VoiceActivityMonitor
{
public:
    using InterruptHandler = std::function<void()>;

    VoiceActivityMonitor() = default;
    ~VoiceActivityMonitor();

    VoiceActivityMonitor(const VoiceActivityMonitor&) = delete;
    VoiceActivityMonitor& operator=(const VoiceActivityMonitor&) = delete;

    void Configure(const bargeInSettings& settings, int sampleRate);
    void SetEnabled(bool enabled);
    [[nodiscard]] bool IsEnabled() const;

    // Begins watching. The handler fires at most once per Begin/End pair, on the monitor
    // thread, and only after sustained input above the threshold.
    void Begin(InterruptHandler handler);
    void End();

    [[nodiscard]] bool IsWatching() const;
    [[nodiscard]] bool Triggered() const;

    // Root-mean-square of one frame of 16-bit mono samples. Separated so the detection
    // rule can be tested without a microphone.
    [[nodiscard]] static int FrameEnergy(const std::int16_t* samples, std::size_t count);

    // Detects user speech above a learned speaker/room noise floor.
    // Qualifying frames never update the floor, preventing sustained speech from being learned as noise.
    class Detector
    {
    public:
        explicit Detector(bargeInSettings settings);

        // withinGrace frames only teach the floor; they can never trigger.
        [[nodiscard]] bool Observe(int energy, bool withinGrace);
        [[nodiscard]] int NoiseFloor() const;
        [[nodiscard]] int CurrentThreshold() const;

    private:
        bargeInSettings configuration;
        double floorEstimate = 0.0;
        bool hasFloor = false;
        int consecutive = 0;
    };

private:
    void Run(std::stop_token stopToken, InterruptHandler handler);

    mutable std::mutex mutex;
    bargeInSettings configuration;
    int captureSampleRate = 16000;
    std::jthread worker;
    std::atomic<bool> watching = false;
    std::atomic<bool> triggered = false;
};

} // namespace revia::speech
