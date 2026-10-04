#include "Runtime/responseLatency.h"

#include <iostream>
#include <stdexcept>

namespace
{
void Expect(const bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

revia::runtime::RuntimeEvent Event(
    const revia::runtime::RuntimeStamp& stamp, const std::string& component, const std::string& phase, const std::uint64_t turn)
{
    revia::runtime::RuntimeEvent event;
    event.stamp = stamp;
    event.kind = revia::runtime::RuntimeEventKind::Timing;
    event.component = component;
    event.phase = phase;
    event.turnId = turn;
    return event;
}
}

void RunResponseLatencyTests()
{
    using namespace revia::runtime;
    using namespace std::chrono;
    const RuntimeStamp stamp{"latency-companion", "latency-session", 1, {}, {}, 0};
    ResponseLatency latency;
    latency.Reset(stamp);
    const auto started = ResponseLatency::Clock::time_point{};
    auto delayed = Event(stamp, "Response latency", "Started", 41);
    delayed.elapsedMilliseconds = 3000;
    latency.Observe(delayed, started + milliseconds(3000));
    latency.Observe(Event(stamp, "Response latency", "TextReady", 41), started + milliseconds(3120));
    Expect(latency.Snapshot().textMilliseconds == 3120, "Accepted input queue and setup time were excluded from response timing.");
    latency.Reset(stamp);
    Expect(latency.Observe(Event(stamp, "Response latency", "Started", 42), started),
        "Admitted input did not begin a correlated response measurement.");
    Expect(latency.Observe(Event(stamp, "Response latency", "TextReady", 42), started + milliseconds(120)),
        "Accepted text was not measured from admitted input.");
    auto snapshot = latency.Snapshot();
    Expect(snapshot.turnId == 42 && snapshot.textMilliseconds == 120 && snapshot.text.samples == 1,
        "Text measurement has the wrong origin or duration.");
    Expect(snapshot.firstAudioPlayedMilliseconds < 0 && snapshot.audioPlayed.samples == 0,
        "Unavailable audio was presented as measured zero latency.");
    Expect(!latency.Observe(Event(stamp, "Voice", "FirstAudioPlayed", 42), started + milliseconds(150)),
        "Matching numeric utterance and turn IDs invented a speech correlation.");

    auto binding = Event(stamp, "Response latency", "SpeechQueued", 42);
    binding.utteranceId = 700;
    Expect(latency.Observe(binding, started + milliseconds(125)), "Speech was not explicitly correlated.");
    Expect(latency.Observe(Event(stamp, "Voice", "FirstAudioReady", 700), started + milliseconds(220)),
        "Correlated playable audio was not measured.");
    Expect(latency.Observe(Event(stamp, "Voice", "FirstAudioPlayed", 700), started + milliseconds(260)),
        "Correlated playback was not measured.");
    snapshot = latency.Snapshot();
    Expect(snapshot.firstAudioReadyMilliseconds == 220 && snapshot.firstAudioPlayedMilliseconds == 260,
        "Audio measurements started at queue time rather than admitted input.");
    Expect(!latency.Observe(Event(stamp, "Voice", "FirstAudioPlayed", 700), started + milliseconds(300)),
        "Duplicate playback notification counted as another first audio.");
    Expect(snapshot.Summary().find("p95") != std::string::npos, "Measured distribution is not observable.");

    Expect(latency.Observe(Event(stamp, "Response latency", "Started", 43), started + milliseconds(400)), "Second turn was not admitted.");
    binding.turnId = 43;
    binding.utteranceId = 701;
    latency.Observe(binding, started + milliseconds(410));
    latency.Observe(Event(stamp, "Response latency", "Cancelled", 43), started + milliseconds(420));
    Expect(!latency.Observe(Event(stamp, "Voice", "FirstAudioPlayed", 701), started + milliseconds(500)),
        "Cancelled turn retained a late audio measurement.");

    const RuntimeStamp next{"latency-companion-b", "latency-session-b", 2, {}, {}, 0};
    latency.Reset(next);
    Expect(!latency.Observe(Event(stamp, "Response latency", "Started", 44), started + milliseconds(600)),
        "Retired session contaminated the new companion's measurements.");
    Expect(latency.Snapshot().text.samples == 0, "Switching companions retained private timing history.");

    for (std::uint64_t index = 1; index <= 100; ++index)
    {
        latency.Observe(Event(next, "Response latency", "Started", index), started);
        latency.Observe(Event(next, "Response latency", "TextReady", index), started + milliseconds(index));
    }
    snapshot = latency.Snapshot();
    Expect(snapshot.text.samples == 64 && snapshot.text.p95Milliseconds == 97, "Timing sample window or nearest-rank p95 is incorrect.");
}

#ifdef REVIA_RESPONSE_LATENCY_STANDALONE
int main()
{
    try
    {
        RunResponseLatencyTests();
        std::cout << "Response latency checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Response latency checks failed: " << error.what() << '\n';
        return 1;
    }
}
#endif
