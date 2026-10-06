#include "Presence/presenceRuntime.h"
#include "Speech/playbackEnvelope.h"
#include "testSupport.h"

#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>

void RunPlaybackPresenceTests()
{
    using revia::tests::Check;
    revia::tests::ScopedTestDirectory directory;
    presenceSettings settings;
    settings.statePath = (directory.root / "avatar_state.json").string();
    settings.eventPath = (directory.root / "avatar_events.jsonl").string();
    revia::presence::PresenceRuntime presence;
    Check(presence.Start(settings, {}, {}), "Disposable avatar producer did not start.");
    const auto read = [&]
    {
        std::ifstream input(settings.statePath);
        return nlohmann::json::parse(input);
    };
    auto track = std::make_shared<revia::speech::PlaybackEnvelope>();
    track->startedAtUnixMs = 1780000000000;
    track->values = {0, 128, 255, 0};
    revia::runtime::RuntimeEvent voice;
    voice.kind = revia::runtime::RuntimeEventKind::ComponentStatus;
    voice.component = "Voice";
    voice.phase = "Speaking";
    voice.utteranceId = 1;
    voice.playbackEnvelope = track;
    presence.Observe(voice);
    Check(read().at("mouth_track").at("values") == nlohmann::json({0, 128, 255, 0}),
        "The actual speaking event lost its measured output track.");

    voice.phase = "Generating";
    voice.utteranceId = 2;
    voice.playbackEnvelope.reset();
    presence.Observe(voice);
    Check(read().at("phase") == "responding" && read().at("speaking") == true && read().contains("mouth_track"),
        "Preparing the next phrase stopped the earlier audio's mouth track.");
    voice.phase = "Ready";
    presence.Observe(voice);
    Check(read().contains("mouth_track") && read().at("speaking") == true,
        "A different utterance's terminal event cleared active playback.");
    voice.utteranceId = 1;
    presence.Observe(voice);
    Check(!read().contains("mouth_track") && read().at("speaking") == false,
        "Playback completion retained an active mouth track.");

    voice.phase = "Speaking";
    voice.playbackEnvelope = track;
    presence.Observe(voice);
    const auto priorSequence = read().at("sequence").get<std::uint64_t>();
    auto nextTrack = std::make_shared<revia::speech::PlaybackEnvelope>(*track);
    nextTrack->startedAtUnixMs += 200;
    voice.playbackEnvelope = nextTrack;
    presence.Observe(voice);
    Check(read().at("sequence").get<std::uint64_t>() > priorSequence &&
        read().at("mouth_track").at("started_at_ms") == nextTrack->startedAtUnixMs,
        "Consecutive speaking phrases did not replace their playback clock.");
    voice.phase = "Interrupted";
    voice.playbackEnvelope.reset();
    presence.Observe(voice);
    Check(!read().contains("mouth_track") && read().at("speaking") == false,
        "Interruption did not close the output mouth track.");

    voice.phase = "Speaking";
    voice.playbackEnvelope = track;
    presence.Observe(voice);
    voice.phase = "PlaybackEnded";
    voice.utteranceId = 2;
    voice.playbackEnvelope.reset();
    presence.Observe(voice);
    Check(read().contains("mouth_track") && read().at("speaking") == true,
        "A revoked different utterance closed current playback.");
    voice.utteranceId = 1;
    presence.Observe(voice);
    Check(read().at("phase") == "idle" && !read().contains("mouth_track") && read().at("speaking") == false,
        "Revoked active playback left a speaking gate or mouth track open.");

    voice.phase = "Speaking";
    voice.playbackEnvelope = track;
    presence.Observe(voice);
    presence.Shutdown();
    Check(read().at("phase") == "offline" && !read().contains("mouth_track") && read().at("speaking") == false,
        "Session shutdown retained the audio track.");
}
