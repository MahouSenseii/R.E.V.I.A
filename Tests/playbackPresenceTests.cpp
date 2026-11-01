#include "Presence/presenceRuntime.h"
#include "Speech/playbackEnvelope.h"
#include "testSupport.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{
constexpr std::size_t EnvelopeByteLimit = 262144;
constexpr const char* PrivatePayload = "PRIVATE_ADAPTER_PAYLOAD_SENTINEL";

struct AdapterInboxFixture
{
    revia::tests::ScopedTestDirectory directory;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<revia::presence::ExternalAdapterEvent> events;
    std::vector<revia::presence::PresenceNotice> notices;
    revia::presence::PresenceRuntime presence;

    AdapterInboxFixture()
    {
        presenceSettings settings;
        settings.statePath = (directory.root / "avatar_state.json").string();
        settings.eventPath = (directory.root / "avatar_events.jsonl").string();
        settings.inboxPath = (directory.root / "Inbox").string();
        settings.outboxPath = (directory.root / "Outbox").string();
        settings.bExternalAdaptersEnabled = true;
        settings.adapterPollMs = 50;
        settings.maxAdapterTextCharacters = 20000;
        revia::tests::Check(presence.Start(settings,
            [this](const auto& notice)
            {
                {
                    std::lock_guard lock(mutex);
                    notices.push_back(notice);
                }
                condition.notify_all();
            },
            [this](const auto& event)
            {
                {
                    std::lock_guard lock(mutex);
                    events.push_back(event);
                }
                condition.notify_all();
            }), "The disposable adapter inbox did not start.");
    }

    ~AdapterInboxFixture()
    {
        presence.Shutdown();
    }

    void Publish(const std::string& name, const std::string& bytes)
    {
        const auto pending = directory.root / "Inbox" / (name + ".pending");
        {
            std::ofstream file(pending, std::ios::binary);
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            revia::tests::Check(file.good(), "The disposable adapter envelope could not be written.");
        }
        std::filesystem::rename(pending, directory.root / "Inbox" / (name + ".json"));
    }

    void WaitForArchive(const std::string& name)
    {
        std::unique_lock lock(mutex);
        revia::tests::Check(condition.wait_for(lock, std::chrono::seconds(3), [&]
        {
            return std::filesystem::exists(directory.root / "Inbox/Processed" / (name + ".json")) ||
                std::filesystem::exists(directory.root / "Inbox/Rejected" / (name + ".json"));
        }), "The disposable adapter envelope was not handled within its deadline.");
    }

    bool Received(const std::string& id, const std::string& text = {})
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(3), [&]
        {
            return std::any_of(events.begin(), events.end(), [&](const auto& event)
            {
                return event.id == id && (text.empty() || event.text == text);
            });
        });
    }

    std::string RejectionDetail()
    {
        std::unique_lock lock(mutex);
        condition.wait_for(lock, std::chrono::seconds(3), [&]
        {
            return std::any_of(notices.begin(), notices.end(), [](const auto& notice)
            {
                return notice.component == "Adapters" && notice.phase == "Rejected";
            });
        });
        const auto found = std::find_if(notices.rbegin(), notices.rend(), [](const auto& notice)
        {
            return notice.component == "Adapters" && notice.phase == "Rejected";
        });
        return found == notices.rend() ? std::string() : found->detail;
    }
};

std::string PaddedEnvelope(const std::string& id, const std::size_t bytes)
{
    nlohmann::json document = {{"id", id}, {"source", "discord"}, {"text", "A bounded message."}, {"padding", ""}};
    const auto overhead = document.dump().size();
    const std::string sentinel = PrivatePayload;
    revia::tests::Check(bytes >= overhead + sentinel.size(), "The envelope fixture is too small for its metadata.");
    document["padding"] = sentinel + std::string(bytes - overhead - sentinel.size(), 'p');
    const auto result = document.dump();
    revia::tests::Check(result.size() == bytes, "The envelope fixture did not have its requested wire size.");
    return result;
}

void TestAdapterEnvelopeBoundaryAndFollowingEvent()
{
    using revia::tests::Check;
    AdapterInboxFixture fixture;
    fixture.Publish("01-at-limit", PaddedEnvelope("at-limit", EnvelopeByteLimit));
    fixture.WaitForArchive("01-at-limit");
    Check(fixture.Received("at-limit"), "An envelope exactly at the byte boundary was not admitted.");

    fixture.Publish("02-over-limit", PaddedEnvelope("over-limit", EnvelopeByteLimit + 1));
    fixture.WaitForArchive("02-over-limit");
    fixture.Publish("03-following", R"({"id":"following","source":"discord","text":"Still listening."})");
    fixture.WaitForArchive("03-following");
    Check(fixture.Received("following", "Still listening."), "An oversized envelope stopped the next valid adapter event.");
    Check(!fixture.Received("over-limit"), "A short-text envelope over the total byte limit reached the conversation callback.");
    const auto archived = fixture.directory.root / "Inbox/Rejected/02-over-limit.json";
    Check(std::filesystem::is_regular_file(archived) && std::filesystem::file_size(archived) == EnvelopeByteLimit + 1,
        "The oversized envelope was not preserved in the existing rejected archive.");
    const auto detail = fixture.RejectionDetail();
    Check(detail.find("byte") != std::string::npos && detail.find("limit") != std::string::npos &&
        detail.find(PrivatePayload) == std::string::npos, "Oversized input did not produce a metadata-only byte-limit refusal.");
}

void TestOversizedMalformedEnvelopeIsRejectedBeforeJsonParsing()
{
    using revia::tests::Check;
    AdapterInboxFixture fixture;
    std::string bytes = PrivatePayload;
    bytes.resize(EnvelopeByteLimit + 1, '{');
    fixture.Publish("malformed-large", bytes);
    fixture.WaitForArchive("malformed-large");
    const auto detail = fixture.RejectionDetail();
    Check(detail.find("byte") != std::string::npos && detail.find("limit") != std::string::npos &&
        detail.find("JSON") == std::string::npos && detail.find(PrivatePayload) == std::string::npos,
        "An oversized malformed envelope reached JSON parsing instead of the wire byte bound.");
    Check(!fixture.Received("malformed-large"), "A malformed oversized envelope reached the conversation callback.");
}

void TestMaximumEscapedAdapterTextFitsTheWireBudget()
{
    AdapterInboxFixture fixture;
    std::string bytes = R"({"id":"escaped-max","source":"discord","text":")";
    std::string expected;
    for (int pair = 0; pair < 10000; ++pair)
    {
        bytes += R"(\u0061\u0062)";
        expected += "ab";
    }
    bytes += R"("})";
    revia::tests::Check(expected.size() == 20000 && bytes.size() > 120000 && bytes.size() < EnvelopeByteLimit,
        "The maximum configured text fixture did not exercise escaped wire overhead.");
    fixture.Publish("escaped-max", bytes);
    fixture.WaitForArchive("escaped-max");
    revia::tests::Check(fixture.Received("escaped-max", expected),
        "Supported maximum-length text was rejected because of its JSON escape overhead.");
}

void TestSmallMalformedEnvelopeStillReportsJsonFailure()
{
    AdapterInboxFixture fixture;
    fixture.Publish("malformed-small", "{not-json");
    fixture.WaitForArchive("malformed-small");
    revia::tests::Check(fixture.RejectionDetail().find("JSON") != std::string::npos,
        "A small malformed envelope lost the existing JSON-error diagnosis.");
}
} // namespace

void RunPlaybackPresenceTests()
{
    using revia::tests::Check;
    revia::tests::ScopedTestDirectory directory;
    presenceSettings settings;
    settings.statePath = (directory.root / "avatar_state.json").string();
    settings.eventPath = (directory.root / "avatar_events.jsonl").string();
    settings.inboxPath = (directory.root / "Inbox").string();
    settings.outboxPath = (directory.root / "Outbox").string();
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

    TestAdapterEnvelopeBoundaryAndFollowingEvent();
    TestOversizedMalformedEnvelopeIsRejectedBeforeJsonParsing();
    TestMaximumEscapedAdapterTextFitsTheWireBudget();
    TestSmallMalformedEnvelopeStillReportsJsonFailure();
}
