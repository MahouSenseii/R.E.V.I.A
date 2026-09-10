#include "Performance/performanceRuntime.h"

#include "Core/runtimePath.h"
#include "Performance/wavAudio.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace revia::performance
{

namespace
{

// One song, mixed down to the stereo stream the device will actually receive.
struct MixedSong
{
    bool succeeded = false;
    std::string error;
    int sampleRate = 0;
    std::vector<std::int16_t> samples;
    std::vector<VocalSpan> vocalSpans;
    std::size_t clippedSamples = 0;
    std::int64_t durationMs = 0;
};

MixedSong MixSong(const SongAsset& asset, const PerformanceConfig& config)
{
    MixedSong mixed;
    const std::int64_t maximumFrames = config.maximumSongSeconds > 0
        ? static_cast<std::int64_t>(config.maximumSongSeconds) * 192000 : 0;

    PcmAudio instrumental;
    PcmAudio vocal;
    if (asset.hasInstrumental &&
        !ReadWavFile(asset.instrumentalPath, instrumental, mixed.error, maximumFrames))
    {
        return mixed;
    }
    if (asset.hasVocal && !ReadWavFile(asset.vocalPath, vocal, mixed.error, maximumFrames))
    {
        return mixed;
    }

    // Two tracks at different rates would need resampling, and a hasty resampler sounds
    // worse than an honest refusal that says exactly what to fix.
    if (!instrumental.Empty() && !vocal.Empty() &&
        instrumental.sampleRate != vocal.sampleRate)
    {
        mixed.error = "The two tracks are recorded at different sample rates (" +
            std::to_string(instrumental.sampleRate) + " Hz and " +
            std::to_string(vocal.sampleRate) + " Hz). Export both at the same rate.";
        return mixed;
    }

    mixed.sampleRate = instrumental.Empty() ? vocal.sampleRate : instrumental.sampleRate;
    const int seconds = config.maximumSongSeconds;
    const std::int64_t longestMs = std::max(instrumental.DurationMs(), vocal.DurationMs());
    if (seconds > 0 && longestMs > static_cast<std::int64_t>(seconds) * 1000)
    {
        mixed.error = "That song is longer than the configured limit of " +
            std::to_string(seconds) + " seconds.";
        return mixed;
    }

    // Measured on the vocal alone, before it is buried in the backing track. Doing it
    // after the mix would report the instrumental's loudness as singing.
    if (!vocal.Empty())
    {
        mixed.vocalSpans = DetectVocalSpans(vocal);
    }

    if (!instrumental.Empty() && !ConvertToStereo(instrumental, mixed.error))
    {
        return mixed;
    }
    if (!vocal.Empty() && !ConvertToStereo(vocal, mixed.error))
    {
        return mixed;
    }

    const double instrumentalGain = asset.metadata.instrumentalGain * config.instrumentalGain;
    const double vocalGain = asset.metadata.vocalGain * config.vocalGain;
    if (instrumental.Empty())
    {
        mixed.samples = std::move(vocal.samples);
        ApplyGain(mixed.samples, vocalGain);
    }
    else if (vocal.Empty())
    {
        mixed.samples = std::move(instrumental.samples);
        ApplyGain(mixed.samples, instrumentalGain);
    }
    else
    {
        mixed.samples = std::move(instrumental.samples);
        mixed.clippedSamples =
            MixInto(mixed.samples, vocal.samples, instrumentalGain, vocalGain);
    }

    if (mixed.samples.empty() || mixed.sampleRate <= 0)
    {
        mixed.error = "The song produced no audio to play.";
        return mixed;
    }
    mixed.durationMs =
        (static_cast<std::int64_t>(mixed.samples.size() / 2) * 1000) / mixed.sampleRate;
    mixed.succeeded = true;
    return mixed;
}

} // namespace

std::string ToString(const PerformanceState value)
{
    switch (value)
    {
        case PerformanceState::Preparing: return "preparing";
        case PerformanceState::Performing: return "performing";
        case PerformanceState::Stopping: return "stopping";
        case PerformanceState::Idle:
        default: return "idle";
    }
}

std::string ToString(const PerformanceEventKind value)
{
    switch (value)
    {
        case PerformanceEventKind::SongStarted: return "song_started";
        case PerformanceEventKind::SectionStarted: return "section_started";
        case PerformanceEventKind::VocalStarted: return "vocal_started";
        case PerformanceEventKind::VocalEnded: return "vocal_ended";
        case PerformanceEventKind::SongEnded: return "song_ended";
        case PerformanceEventKind::SongInterrupted: return "song_interrupted";
        case PerformanceEventKind::SongFailed:
        default: return "song_failed";
    }
}

std::string FormatSongTime(const std::int64_t milliseconds)
{
    const std::int64_t total = std::max<std::int64_t>(0, milliseconds) / 1000;
    const std::int64_t seconds = total % 60;
    std::string text = std::to_string(total / 60) + ":";
    if (seconds < 10) text += "0";
    return text + std::to_string(seconds);
}

PerformanceRuntime::PerformanceRuntime() = default;

PerformanceRuntime::~PerformanceRuntime()
{
    Stop("Revia is shutting down.");
    JoinWorker();
}

void PerformanceRuntime::Configure(PerformanceConfig inputConfig)
{
    std::lock_guard lock(mutex);
    config = std::move(inputConfig);
    const std::filesystem::path root = core::ResolveRuntimeWritePath(config.songLibraryPath);
    library.SetRoot(root);
    // Created on startup so there is somewhere obvious to put a song. Failing to create
    // it is not a startup failure: the library simply lists nothing and says why.
    std::error_code error;
    std::filesystem::create_directories(root, error);
}

const PerformanceConfig& PerformanceRuntime::Config() const
{
    return config;
}

const SongLibrary& PerformanceRuntime::Library() const
{
    return library;
}

void PerformanceRuntime::SetObserver(Observer inputObserver)
{
    std::lock_guard lock(mutex);
    observer = std::move(inputObserver);
}

void PerformanceRuntime::Publish(const PerformanceEvent& event) const
{
    Observer copy;
    {
        std::lock_guard lock(mutex);
        copy = observer;
    }
    // Called outside the lock: an observer that publishes onto an event bus should never
    // be able to deadlock the playback thread against a status read.
    if (copy) copy(event);
}

void PerformanceRuntime::SetStatus(const PerformanceStatus& inputStatus)
{
    std::lock_guard lock(mutex);
    status = inputStatus;
}

PerformanceStatus PerformanceRuntime::Status() const
{
    std::lock_guard lock(mutex);
    return status;
}

bool PerformanceRuntime::IsPerforming() const
{
    return performing.load(std::memory_order_acquire);
}

void PerformanceRuntime::JoinWorker()
{
    if (worker.joinable())
    {
        worker.join();
    }
}

void PerformanceRuntime::Stop(const std::string& reason)
{
    if (!worker.joinable())
    {
        return;
    }
    {
        std::lock_guard lock(mutex);
        if (stopReason.empty()) stopReason = reason;
        status.state = PerformanceState::Stopping;
    }
    worker.request_stop();
}

SongRehearsal PerformanceRuntime::Rehearse(const std::string& songQuery) const
{
    SongRehearsal rehearsal;
    std::string songId;
    if (!library.Resolve(songQuery, songId, rehearsal.error))
    {
        return rehearsal;
    }
    SongAsset asset;
    if (!library.Load(songId, asset, rehearsal.error))
    {
        return rehearsal;
    }
    rehearsal.songId = asset.metadata.id;
    rehearsal.title = asset.metadata.title;
    rehearsal.artist = asset.metadata.artist;
    rehearsal.hasInstrumental = asset.hasInstrumental;
    rehearsal.hasVocal = asset.hasVocal;
    rehearsal.sectionCount = asset.metadata.sections.size();

    const MixedSong mixed = MixSong(asset, config);
    if (!mixed.succeeded)
    {
        rehearsal.error = mixed.error;
        return rehearsal;
    }
    rehearsal.sampleRate = mixed.sampleRate;
    rehearsal.durationMs = mixed.durationMs;
    rehearsal.vocalSpanCount = mixed.vocalSpans.size();
    rehearsal.clippedSamples = mixed.clippedSamples;
    rehearsal.totalSamples = mixed.samples.size();
    rehearsal.succeeded = true;
    return rehearsal;
}

bool PerformanceRuntime::Start(const std::string& songQuery, std::string& outError)
{
    if (!config.enabled)
    {
        outError = "Singing is turned off in settings.";
        return false;
    }
    if (IsPerforming())
    {
        std::lock_guard lock(mutex);
        outError = "She is already performing " + status.title + ".";
        return false;
    }
    // A previous song's thread may have finished without anyone joining it.
    JoinWorker();

    std::string songId;
    if (!library.Resolve(songQuery, songId, outError))
    {
        return false;
    }
    SongAsset asset;
    if (!library.Load(songId, asset, outError))
    {
        return false;
    }

    {
        std::lock_guard lock(mutex);
        stopReason.clear();
        status = {};
        status.state = PerformanceState::Preparing;
        status.songId = asset.metadata.id;
        status.title = asset.metadata.title;
        status.artist = asset.metadata.artist;
    }
    performing.store(true, std::memory_order_release);
    worker = std::jthread([this, asset = std::move(asset)](std::stop_token stopToken) mutable
    {
        Perform(std::move(stopToken), std::move(asset));
    });
    outError.clear();
    return true;
}

void PerformanceRuntime::Perform(std::stop_token stopToken, SongAsset asset)
{
    PerformanceEvent event;
    event.songId = asset.metadata.id;
    event.title = asset.metadata.title;

    // Everything from here is guarded so a decode fault, a missing device, or a bad
    // header ends this thread and nothing else. Singing failing must not take speech,
    // conversation, or the session down with it.
    const auto finish = [&](const PerformanceEventKind kind, const std::string& message,
        const std::int64_t positionMs, const std::int64_t durationMs)
    {
        {
            std::lock_guard lock(mutex);
            status.state = PerformanceState::Idle;
            status.positionMs = positionMs;
            status.line.clear();
            status.vocalActive = false;
        }
        performing.store(false, std::memory_order_release);
        PerformanceEvent ending = event;
        ending.kind = kind;
        ending.message = message;
        ending.positionMs = positionMs;
        ending.durationMs = durationMs;
        Publish(ending);
    };

    MixedSong mixed;
    try
    {
        mixed = MixSong(asset, config);
    }
    catch (const std::exception& failure)
    {
        finish(PerformanceEventKind::SongFailed,
            std::string("The song could not be prepared: ") + failure.what(), 0, 0);
        return;
    }
    catch (...)
    {
        finish(PerformanceEventKind::SongFailed, "The song could not be prepared.", 0, 0);
        return;
    }
    if (!mixed.succeeded)
    {
        finish(PerformanceEventKind::SongFailed, mixed.error, 0, 0);
        return;
    }
    if (stopToken.stop_requested())
    {
        std::string reason;
        {
            std::lock_guard lock(mutex);
            reason = stopReason;
        }
        finish(PerformanceEventKind::SongInterrupted,
            reason.empty() ? "The song was stopped." : reason, 0, mixed.durationMs);
        return;
    }

    {
        std::lock_guard lock(mutex);
        status.state = PerformanceState::Performing;
        status.durationMs = mixed.durationMs;
    }
    event.durationMs = mixed.durationMs;
    PerformanceEvent started = event;
    started.kind = PerformanceEventKind::SongStarted;
    started.message = asset.metadata.artist.empty()
        ? asset.metadata.title
        : asset.metadata.title + " - " + asset.metadata.artist;
    Publish(started);

#ifdef _WIN32
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = static_cast<DWORD>(mixed.sampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEOUT device = nullptr;
    if (waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    {
        finish(PerformanceEventKind::SongFailed,
            "Windows could not open an audio device for playback.", 0, mixed.durationMs);
        return;
    }

    constexpr int BufferCount = 4;
    const std::int64_t framesPerBuffer = std::max<std::int64_t>(
        512, (static_cast<std::int64_t>(mixed.sampleRate) *
            std::clamp(config.outputBufferMs, 20, 1000)) / 1000);
    const std::size_t samplesPerBuffer = static_cast<std::size_t>(framesPerBuffer) * 2;

    std::vector<std::vector<std::int16_t>> buffers(BufferCount);
    std::vector<WAVEHDR> headers(BufferCount);
    for (int index = 0; index < BufferCount; ++index)
    {
        buffers[index].resize(samplesPerBuffer);
        headers[index] = {};
    }

    std::size_t nextSample = 0;
    const std::size_t totalSamples = mixed.samples.size();
    std::size_t sectionIndex = 0;
    std::size_t vocalIndex = 0;
    bool vocalActive = false;
    bool interrupted = false;
    std::int64_t positionMs = 0;

    const auto submit = [&](const int index) -> bool
    {
        if (nextSample >= totalSamples)
        {
            return false;
        }
        const std::size_t count = std::min(samplesPerBuffer, totalSamples - nextSample);
        std::copy_n(mixed.samples.begin() + static_cast<std::ptrdiff_t>(nextSample),
            count, buffers[index].begin());
        nextSample += count;
        WAVEHDR& header = headers[index];
        header = {};
        header.lpData = reinterpret_cast<LPSTR>(buffers[index].data());
        header.dwBufferLength = static_cast<DWORD>(count * sizeof(std::int16_t));
        if (waveOutPrepareHeader(device, &header, sizeof(header)) != MMSYSERR_NOERROR)
        {
            return false;
        }
        return waveOutWrite(device, &header, sizeof(header)) == MMSYSERR_NOERROR;
    };

    for (int index = 0; index < BufferCount; ++index)
    {
        if (!submit(index)) break;
    }

    while (!stopToken.stop_requested())
    {
        // The device's own sample counter, not the count of what has been handed to it:
        // one is where the music actually is, the other is up to half a second ahead.
        MMTIME time{};
        time.wType = TIME_SAMPLES;
        if (waveOutGetPosition(device, &time, sizeof(time)) == MMSYSERR_NOERROR &&
            time.wType == TIME_SAMPLES)
        {
            positionMs = (static_cast<std::int64_t>(time.u.sample) * 1000) / mixed.sampleRate;
        }

        while (sectionIndex < asset.metadata.sections.size() &&
            asset.metadata.sections[sectionIndex].startMs <= positionMs)
        {
            const SongSection& section = asset.metadata.sections[sectionIndex];
            {
                std::lock_guard lock(mutex);
                status.sectionLabel = section.label;
                status.line = section.line;
                status.positionMs = positionMs;
            }
            PerformanceEvent cue = event;
            cue.kind = PerformanceEventKind::SectionStarted;
            cue.label = section.label;
            cue.line = section.line;
            cue.positionMs = positionMs;
            cue.message = section.line.empty() ? section.label : section.line;
            Publish(cue);
            ++sectionIndex;
        }

        while (vocalIndex < mixed.vocalSpans.size())
        {
            const VocalSpan& span = mixed.vocalSpans[vocalIndex];
            if (!vocalActive && positionMs >= span.startMs)
            {
                vocalActive = true;
                {
                    std::lock_guard lock(mutex);
                    status.vocalActive = true;
                }
                PerformanceEvent vocalEvent = event;
                vocalEvent.kind = PerformanceEventKind::VocalStarted;
                vocalEvent.positionMs = positionMs;
                Publish(vocalEvent);
                break;
            }
            if (vocalActive && positionMs >= span.endMs)
            {
                vocalActive = false;
                {
                    std::lock_guard lock(mutex);
                    status.vocalActive = false;
                }
                PerformanceEvent vocalEvent = event;
                vocalEvent.kind = PerformanceEventKind::VocalEnded;
                vocalEvent.positionMs = positionMs;
                Publish(vocalEvent);
                ++vocalIndex;
                continue;
            }
            break;
        }

        {
            std::lock_guard lock(mutex);
            status.positionMs = positionMs;
        }

        bool anyPending = false;
        for (int index = 0; index < BufferCount; ++index)
        {
            WAVEHDR& header = headers[index];
            if (header.dwBufferLength == 0)
            {
                continue;
            }
            if ((header.dwFlags & WHDR_DONE) != 0)
            {
                waveOutUnprepareHeader(device, &header, sizeof(header));
                header.dwBufferLength = 0;
                if (!submit(index))
                {
                    continue;
                }
            }
            anyPending = true;
        }
        if (!anyPending && nextSample >= totalSamples)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    interrupted = stopToken.stop_requested();
    waveOutReset(device);
    for (WAVEHDR& header : headers)
    {
        if (header.dwBufferLength != 0)
        {
            waveOutUnprepareHeader(device, &header, sizeof(header));
        }
    }
    waveOutClose(device);
#else
    const bool interrupted = stopToken.stop_requested();
    std::int64_t positionMs = 0;
#endif

    if (interrupted)
    {
        std::string reason;
        {
            std::lock_guard lock(mutex);
            reason = stopReason;
        }
        finish(PerformanceEventKind::SongInterrupted,
            reason.empty() ? "The song was stopped." : reason, positionMs, mixed.durationMs);
        return;
    }
    finish(PerformanceEventKind::SongEnded, "The song finished.",
        mixed.durationMs, mixed.durationMs);
}

} // namespace revia::performance
