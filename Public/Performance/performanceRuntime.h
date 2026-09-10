#pragma once

#include "Performance/songLibrary.h"
#include "Performance/songTypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace revia::performance
{

struct PerformanceConfig
{
    bool enabled = true;
    std::string songLibraryPath = "RuntimeData/Songs";
    // One voice. When she is asked something while performing, she stops singing to
    // answer rather than talking over herself. Turning this off keeps the song running
    // and leaves the reply on screen only.
    bool interruptSongToSpeak = true;
    int maximumSongSeconds = 600;
    // How much audio is queued to the device at a time. Larger survives a busy machine;
    // smaller makes the stop and the cue timing tighter.
    int outputBufferMs = 120;
    double instrumentalGain = 1.0;
    double vocalGain = 1.0;
};

// What loading and mixing a song would produce, without playing it.
//
// The rehearsal exists for the same reason the action pipeline has a dry run: the
// expensive, failure-prone half of performing a song is reading and mixing it, and being
// able to check that half without filling the room with sound makes it testable and
// makes "will this song work?" answerable before committing to three minutes of audio.
struct SongRehearsal
{
    bool succeeded = false;
    std::string error;
    std::string songId;
    std::string title;
    std::string artist;
    int sampleRate = 0;
    std::int64_t durationMs = 0;
    bool hasInstrumental = false;
    bool hasVocal = false;
    std::size_t sectionCount = 0;
    std::size_t vocalSpanCount = 0;
    // Samples that hit the 16-bit limit while mixing. A handful is inaudible; a large
    // fraction means the two tracks are fighting and the gains need lowering.
    std::size_t clippedSamples = 0;
    std::size_t totalSamples = 0;
};

// The performance owner.
//
// It owns one song at a time, its own playback thread, and its own audio device. It does
// not touch SpeechService, the speech queue, or any model: a song that fails to load, a
// missing audio device, and a corrupt WAV all end as a typed result here and leave
// ordinary conversation and speech exactly as they were.
//
// ReviaSession remains the lifecycle owner; this is started, stopped, and observed by it.
class PerformanceRuntime
{
public:
    PerformanceRuntime();
    ~PerformanceRuntime();

    PerformanceRuntime(const PerformanceRuntime&) = delete;
    PerformanceRuntime& operator=(const PerformanceRuntime&) = delete;

    void Configure(PerformanceConfig config);
    [[nodiscard]] const PerformanceConfig& Config() const;
    [[nodiscard]] const SongLibrary& Library() const;

    // Called from the playback thread. It must not block, and must not call back into
    // this runtime.
    using Observer = std::function<void(const PerformanceEvent&)>;
    void SetObserver(Observer observer);

    // Begins a performance. Returns once the song has been accepted, not once it has
    // finished: loading, mixing, and playing all happen on the playback thread so a
    // three-minute song never blocks a conversation turn.
    [[nodiscard]] bool Start(const std::string& songQuery, std::string& outError);
    // Stops whatever is playing. Safe to call when nothing is. The reason travels into
    // the SongInterrupted event so the transcript says why the music stopped.
    void Stop(const std::string& reason);
    [[nodiscard]] bool IsPerforming() const;
    [[nodiscard]] PerformanceStatus Status() const;

    // Load and mix without playing. Synchronous, and safe to call while a song is
    // performing because it touches no shared playback state.
    [[nodiscard]] SongRehearsal Rehearse(const std::string& songQuery) const;

private:
    void Perform(std::stop_token stopToken, SongAsset asset);
    void Publish(const PerformanceEvent& event) const;
    void SetStatus(const PerformanceStatus& status);
    void JoinWorker();

    mutable std::mutex mutex;
    PerformanceConfig config;
    SongLibrary library;
    Observer observer;
    PerformanceStatus status;
    std::string stopReason;
    std::atomic<bool> performing{false};
    std::jthread worker;
};

} // namespace revia::performance
