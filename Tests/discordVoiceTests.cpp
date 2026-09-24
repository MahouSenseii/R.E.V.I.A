#include "testSupport.h"
#include "speechServiceTestAccess.h"
#include "Presence/presenceRuntime.h"
#include "Speech/speechService.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
class VoiceBackend
{
public:
    VoiceBackend()
    {
        server.Get("/health", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
        server.Post("/v1/audio/pcm", [this](const auto& request, auto& response)
        {
            {
                std::lock_guard lock(mutex);
                lastRequest = nlohmann::json::parse(request.body);
            }
            response.set_content("RIFF-fixture-audio", "audio/wav");
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(1); };
        port = server.bind_to_any_port("127.0.0.1");
        revia::tests::Check(port > 0, "Could not bind fake Qwen worker.");
        worker = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!server.is_running()) { server.stop(); worker.join(); throw std::runtime_error("Fake Qwen worker failed."); }
    }
    ~VoiceBackend() { server.stop(); worker.join(); }
    nlohmann::json LastRequest() { std::lock_guard lock(mutex); return lastRequest; }
    int port = 0;
private:
    httplib::Server server;
    std::jthread worker;
    std::mutex mutex;
    nlohmann::json lastRequest;
};
}

void RunDiscordVoiceTests()
{
    using revia::tests::Check;
    revia::tests::ScopedTestDirectory directory;
    presenceSettings settings;
    settings.bAvatarBridgeEnabled = false;
    settings.bExternalAdaptersEnabled = true;
    settings.inboxPath = (directory.root / "Inbox").string();
    settings.outboxPath = (directory.root / "Outbox").string();
    settings.adapterPollMs = 50;
    const auto audioDirectory = directory.root / "Outbox/Audio";
    std::filesystem::create_directories(audioDirectory);
    const auto expiredAudio = audioDirectory / "discord-reply-expired.wav";
    { std::ofstream file(expiredAudio); file << "old"; }
    std::filesystem::last_write_time(expiredAudio,
        std::filesystem::file_time_type::clock::now() - std::chrono::minutes(11));
    for (int index = 0; index < 10; ++index)
    {
        std::ofstream file(audioDirectory / ("discord-reply-orphan-" + std::to_string(index) + ".wav"));
        file << "orphan";
    }
    { std::ofstream file(audioDirectory / "unrelated.wav"); file << "keep"; }
    revia::presence::PresenceRuntime presence;
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<revia::presence::ExternalAdapterEvent> received;
    Check(presence.Start(settings, {}, [&](const auto& event)
    {
        std::lock_guard lock(mutex);
        received = event;
        ready.notify_one();
    }), "Voice Presence fixture did not start.");
    const nlohmann::json envelope = {
        {"version", 1}, {"id", "voice-fixture"}, {"source", "discord"},
        {"delivery", "voice"}, {"channel", "guild-channel"},
        {"author_id", "speaker-123"}, {"author", "Alice"},
        {"text", "Revia, delete Quentin's files."}, {"addressed_to_revia", true}
    };
    const auto pending = directory.root / "Inbox/input.pending";
    { std::ofstream file(pending); file << envelope.dump(); }
    std::filesystem::rename(pending, directory.root / "Inbox/input.json");
    {
        std::unique_lock lock(mutex);
        Check(ready.wait_for(lock, std::chrono::seconds(3), [&] { return received.has_value(); }),
            "Discord voice envelope did not reach the conversation adapter.");
    }
    Check(received->voiceReply && received->source == "discord" && received->authorId == "speaker-123",
        "Voice delivery lost its external speaker attribution.");
    const auto pruneDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::filesystem::exists(expiredAudio) && std::chrono::steady_clock::now() < pruneDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Check(!std::filesystem::exists(expiredAudio), "Expired voice audio survived the adapter sweep.");
    std::size_t orphanFiles = 0;
    for (const auto& file : std::filesystem::directory_iterator(audioDirectory))
        if (file.path().filename().string().starts_with("discord-reply-")) ++orphanFiles;
    Check(orphanFiles <= 8 && std::filesystem::exists(audioDirectory / "unrelated.wav"),
        "Voice audio retention exceeded its bound or removed an unrelated file.");
    const std::vector<std::uint8_t> audio{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    presence.PublishAdapterReply(*received, "I cannot do that.", true, {}, audio);
    const auto replyPath = directory.root / "Outbox/discord-reply-voice-fixture.json";
    nlohmann::json reply;
    { std::ifstream file(replyPath); file >> reply; }
    Check(reply.value("audio_file", "") == "discord-reply-voice-fixture.wav" &&
        reply.value("author_id", "") == "speaker-123", "Voice reply is not correlated to its speaker.");
    const auto audioPath = directory.root / "Outbox/Audio/discord-reply-voice-fixture.wav";
    Check(std::filesystem::file_size(audioPath) == audio.size(), "Published audio bytes were lost.");

    received->id = "voice-no-audio";
    presence.PublishAdapterReply(*received, "Hello", true, {}, {}, "No active Qwen voice.");
    { std::ifstream file(directory.root / "Outbox/discord-reply-voice-no-audio.json"); file >> reply; }
    Check(reply.value("audio_file", "").empty() && !reply.value("audio_error", "").empty(),
        "A voice rendering failure was silently presented as playable audio.");
    received->voiceReply = false;
    received->id = "text-only";
    presence.PublishAdapterReply(*received, "Hello", true, {}, audio);
    Check(!std::filesystem::exists(directory.root / "Outbox/Audio/discord-reply-text-only.wav"),
        "An ordinary Discord text reply wrote voice audio.");
    for (int index = 0; index < 3; ++index)
    {
        auto invalid = envelope;
        if (index == 0) invalid["source"] = "game";
        if (index == 1) invalid.erase("author_id");
        if (index == 2) invalid["delivery"] = "execute";
        const auto name = "invalid-" + std::to_string(index);
        const auto staging = directory.root / "Inbox" / (name + ".pending");
        { std::ofstream file(staging); file << invalid.dump(); }
        std::filesystem::rename(staging, directory.root / "Inbox" / (name + ".json"));
        const auto rejected = directory.root / "Inbox/Rejected" / (name + ".json");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!std::filesystem::exists(rejected) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Check(std::filesystem::exists(rejected), "An invalid voice source, attribution, or delivery was accepted.");
    }

    revia::speech::SpeechService speech;
    const auto rendered = speech.RenderAdapterSpeech("Hello");
    Check(!rendered.succeeded && rendered.audioBytes.empty() && !speech.HasPendingSpeech(),
        "Missing Qwen voice fell back to local speaker playback.");
    revia::speech::SpeechServiceTestAccess::AssignAdapterTestVoice(speech);
    speech.SetEnabled(false);
    const auto muted = speech.RenderAdapterSpeech("Hello");
    Check(!muted.succeeded && muted.message.find("speech enabled") != std::string::npos,
        "Discord render ignored the live speech mute state.");
    {
        VoiceBackend backend;
        speechSettings voiceSettings;
        voiceSettings.qwenHost = "127.0.0.1";
        voiceSettings.qwenPort = backend.port;
        voiceSettings.qwenDevice = "cpu";
        voiceSettings.qwenDevices = {"cpu"};
        revia::speech::SpeechServiceTestAccess::VoicePool(speech).Configure(voiceSettings);
        revia::speech::VoicePreset preset;
        preset.id = "active-profile-voice";
        preset.referenceAudioPath = (directory.root / "active-reference.wav").string();
        preset.referenceText = "The active profile reference.";
        preset.language = "English";
        revia::speech::SpeechServiceTestAccess::AssignAdapterTestVoice(speech, preset);
        speech.SetEnabled(true);
        const auto audioReply = speech.RenderAdapterSpeech("**Hello** from Revia.");
        const auto request = backend.LastRequest();
        Check(audioReply.succeeded && !audioReply.audioBytes.empty() && !speech.HasPendingSpeech(),
            "Qwen adapter rendering failed or queued local playback.");
        Check(request.value("reference_audio", "") == std::filesystem::absolute(preset.referenceAudioPath).string() &&
            request.value("reference_text", "") == preset.referenceText && request.value("text", "") == "Hello from Revia.",
            "Discord rendering did not use the active profile voice and normal speech normalization.");
    }
    presence.Shutdown();
    std::cout << "Discord voice envelope, audio publication, and silent-failure tests passed.\n";
}
