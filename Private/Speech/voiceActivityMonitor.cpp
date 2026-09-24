#include "Speech/voiceActivityMonitor.h"

#include <array>
#include <chrono>
#include <cmath>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace revia::speech
{

VoiceActivityMonitor::~VoiceActivityMonitor()
{
    End();
}

void VoiceActivityMonitor::Configure(const bargeInSettings& settings, const int sampleRate)
{
    std::lock_guard lock(mutex);
    configuration = settings;
    captureSampleRate = sampleRate > 0 ? sampleRate : 16000;
}

void VoiceActivityMonitor::SetEnabled(const bool enabled)
{
    std::lock_guard lock(mutex);
    configuration.bEnabled = enabled;
}

bool VoiceActivityMonitor::IsEnabled() const
{
    std::lock_guard lock(mutex);
    return configuration.bEnabled;
}

int VoiceActivityMonitor::FrameEnergy(const std::int16_t* samples, const std::size_t count)
{
    if (samples == nullptr || count == 0)
    {
        return 0;
    }
    double sum = 0.0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const double value = static_cast<double>(samples[index]);
        sum += value * value;
    }
    return static_cast<int>(std::sqrt(sum / static_cast<double>(count)));
}

VoiceActivityMonitor::Detector::Detector(bargeInSettings settings)
    : configuration(std::move(settings))
{
}

int VoiceActivityMonitor::Detector::NoiseFloor() const
{
    return static_cast<int>(floorEstimate);
}

int VoiceActivityMonitor::Detector::CurrentThreshold() const
{
    const int adaptive = static_cast<int>(floorEstimate * configuration.echoMarginMultiplier);
    return std::max(configuration.energyThreshold, adaptive);
}

bool VoiceActivityMonitor::Detector::Observe(const int energy, const bool withinGrace)
{
    if (withinGrace || !hasFloor)
    {
        // Converge quickly: the grace window is short and has to arrive at a usable
        // estimate of what Revia's own playback measures before it ends.
        floorEstimate = hasFloor ? (floorEstimate * 0.75) + (energy * 0.25) : energy;
        hasFloor = true;
        return false;
    }

    if (energy >= CurrentThreshold())
    {
        ++consecutive;
        // Deliberately no floor update here. Learning from qualifying frames is how a
        // detector talks itself out of noticing someone who keeps talking.
        return consecutive >= configuration.consecutiveFramesRequired;
    }

    // A single quiet frame resets the run. Speech is sustained; a door closing is not.
    consecutive = 0;
    // Slow drift only, so the floor follows the room and Revia's varying volume without
    // chasing a genuine interruption.
    floorEstimate = (floorEstimate * 0.95) + (energy * 0.05);
    return false;
}

void VoiceActivityMonitor::Begin(InterruptHandler handler)
{
    End();
    bool enabled = false;
    {
        std::lock_guard lock(mutex);
        enabled = configuration.bEnabled;
    }
    if (!enabled || !handler)
    {
        return;
    }
    triggered.store(false);
    worker = std::jthread([this, handler = std::move(handler)](const std::stop_token stopToken)
    {
        Run(stopToken, handler);
    });
}

void VoiceActivityMonitor::Run(const std::stop_token stopToken, InterruptHandler handler)
{
#ifdef _WIN32
    bargeInSettings settings;
    int sampleRate = 16000;
    {
        std::lock_guard lock(mutex);
        settings = configuration;
        sampleRate = captureSampleRate;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEIN input = nullptr;
    if (waveInOpen(&input, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    {
        // No microphone, or it is already in use by the recogniser. Barge-in is a
        // convenience; failing to arm it must never break speaking.
        return;
    }

    constexpr std::size_t BufferCount = 4;
    constexpr std::size_t BufferBytes = 1600;
    std::array<std::array<char, BufferBytes>, BufferCount> storage{};
    std::array<WAVEHDR, BufferCount> headers{};
    for (std::size_t index = 0; index < BufferCount; ++index)
    {
        headers[index].lpData = storage[index].data();
        headers[index].dwBufferLength = static_cast<DWORD>(BufferBytes);
        waveInPrepareHeader(input, &headers[index], sizeof(WAVEHDR));
        waveInAddBuffer(input, &headers[index], sizeof(WAVEHDR));
    }

    waveInStart(input);
    watching.store(true);
    const auto armedAt = std::chrono::steady_clock::now();
    Detector detector(settings);
    bool fired = false;

    while (!stopToken.stop_requested() && !fired)
    {
        for (WAVEHDR& header : headers)
        {
            if ((header.dwFlags & WHDR_DONE) == 0)
            {
                continue;
            }
            if (header.dwBytesRecorded > 0)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - armedAt).count();
                const int energy = FrameEnergy(
                    reinterpret_cast<const std::int16_t*>(header.lpData),
                    header.dwBytesRecorded / sizeof(std::int16_t));
                // Grace frames still feed the detector: that is when it learns what
                // Revia's own playback measures through this microphone.
                if (detector.Observe(energy, elapsed < settings.startupGraceMs))
                {
                    fired = true;
                }
            }
            header.dwBytesRecorded = 0;
            header.dwFlags &= ~WHDR_DONE;
            waveInAddBuffer(input, &header, sizeof(WAVEHDR));
            if (fired)
            {
                break;
            }
        }
        if (!fired)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    waveInStop(input);
    waveInReset(input);
    for (WAVEHDR& header : headers)
    {
        waveInUnprepareHeader(input, &header, sizeof(WAVEHDR));
    }
    waveInClose(input);
    watching.store(false);

    if (fired && !stopToken.stop_requested())
    {
        triggered.store(true);
        handler();
    }
#else
    (void)stopToken;
    (void)handler;
#endif
}

void VoiceActivityMonitor::End()
{
    if (worker.joinable())
    {
        worker.request_stop();
        worker.join();
    }
    watching.store(false);
}

bool VoiceActivityMonitor::IsWatching() const
{
    return watching.load();
}

bool VoiceActivityMonitor::Triggered() const
{
    return triggered.load();
}

} // namespace revia::speech
