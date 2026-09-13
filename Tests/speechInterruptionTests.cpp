#include "testSupport.h"

#include "speechServiceTestAccess.h"

#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>



namespace
{

using namespace revia::speech;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

std::string ReadBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes)
{
    std::ofstream file(path, std::ios::binary);
    file << bytes;
    Check(file.good(), "Could not create speech interruption fixture.");
}

std::filesystem::path BankClip(const std::filesystem::path& root)
{
    const auto directory = root / "fixture-voice" / "vocalizations";
    std::filesystem::create_directories(directory);
    return directory / "soft-laugh-1.wav";
}

VoicePreset FixtureVoice()
{
    VoicePreset voice;
    voice.id = "fixture-voice";
    return voice;
}

void TestInterruptionUsesThePreparedAudioLifetime(const bool bargeIn)
{
    ScopedTestDirectory directory;
    const auto clip = BankClip(directory.root);
    // Real audio, because the bank only offers a clip it could actually play. The
    // bytes are read back so the survival checks below still compare exact content.
    revia::tests::WriteMinimalWav(clip);
    const std::string original = ReadBytes(clip);
    const auto scratch = directory.root / "temporary-phrase.wav";
    WriteBytes(scratch, "temporary generated phrase");

    SpeechService service;
    SpeechServiceTestAccess::ConfigureWithoutWorkers(service, directory.root);
    service.UseVoice(FixtureVoice());
    service.Speak("Before. *chuckles* After.", {}, 41);
    SpeechServiceTestAccess::TakeNext(service)(); // preceding speech
    SpeechServiceTestAccess::TakeNext(service)(); // actual bank preparation
    Check(SpeechServiceTestAccess::HasPreparedClip(service, clip),
        "Speak -> SynthesizeOne did not prepare a borrowed bank clip.");
    SpeechServiceTestAccess::AddTemporaryAudio(service, scratch);

    if (bargeIn) service.YieldToUser();
    else service.StopSpeaking();

    Check(ReadBytes(clip) == original,
        bargeIn ? "Barge-in deleted or changed the persistent vocalization clip."
                : "Stop deleted or changed the persistent vocalization clip.");
    Check(!std::filesystem::exists(scratch),
        "Interruption failed to release temporary generated audio.");
    Check(SpeechServiceTestAccess::QueuesCleared(service),
        "Interruption left queued/prepared speech or buffered-byte accounting behind.");
    Check(service.IsEnabled(), "Interruption disabled voice output for the next turn.");

    // Cancellation must not weaken the existing immediate-repeat policy.
    service.Speak("*chuckles*", {}, 42);
    Check(SpeechServiceTestAccess::QueuesCleared(service),
        "Interruption bypassed the existing vocalization repeat limit.");

    SpeechService nextSession;
    SpeechServiceTestAccess::ConfigureWithoutWorkers(nextSession, directory.root);
    nextSession.UseVoice(FixtureVoice());
    nextSession.Speak("*chuckles*", {}, 44);
    SpeechServiceTestAccess::TakeNext(nextSession)();
    Check(SpeechServiceTestAccess::HasPreparedClip(nextSession, clip),
        "A fresh service could not reuse the interrupted voice's bank.");
    nextSession.StopSpeaking();
    Check(ReadBytes(clip) == original, "Reusing the bank changed the persistent clip.");
}

void TestLatePreparationAfterBargeInPreservesTheBank()
{
    ScopedTestDirectory directory;
    const auto clip = BankClip(directory.root);
    revia::tests::WriteMinimalWav(clip);
    const std::string original = ReadBytes(clip);

    SpeechService service;
    SpeechServiceTestAccess::ConfigureWithoutWorkers(service, directory.root);
    service.UseVoice(FixtureVoice());
    service.Speak("*chuckles*", {}, 43);
    auto complete = SpeechServiceTestAccess::TakeNext(service);
    service.YieldToUser();
    complete();

    Check(ReadBytes(clip) == original,
        "Completion of an interrupted generation deleted the borrowed clip.");
    Check(SpeechServiceTestAccess::QueuesCleared(service),
        "A cancelled generation published late speech after barge-in.");
}

bool WaitFor(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

} // namespace

void RunSpeechInterruptionTests()
{
    TestInterruptionUsesThePreparedAudioLifetime(false);
    TestInterruptionUsesThePreparedAudioLifetime(true);
    TestLatePreparationAfterBargeInPreservesTheBank();
    std::cout << "Speech interruption preserves prepared bank assets, releases scratch audio, "
        "clears the real queues and rejects late preparation.\n";
}

void RunSpeechInterruptionLive(const std::filesystem::path& sourceClip)
{
#ifndef _WIN32
    (void)sourceClip;
    throw std::runtime_error("The live speech interruption check requires Windows.");
#else
    const std::string original = ReadBytes(sourceClip);
    Check(original.size() >= 44 && original.substr(0, 4) == "RIFF" &&
        original.substr(8, 4) == "WAVE", "Supply an existing valid WAV bank clip.");

    ScopedTestDirectory directory;
    const auto clip = BankClip(directory.root);
    std::filesystem::copy_file(sourceClip, clip);
    speechSettings settings;
    settings.bEnabled = true;
    settings.backend = "WindowsSapi";
    settings.voiceDataPath = directory.root.string();

    std::mutex eventMutex;
    std::vector<SpeechEvent> events;
    const auto saw = [&](const std::string& phase, const std::uint64_t id)
    {
        std::lock_guard lock(eventMutex);
        for (const auto& event : events)
            if (event.phase == phase && event.utteranceId == id) return true;
        return false;
    };

    const auto receive = [&](const SpeechEvent& event)
    {
        std::lock_guard lock(eventMutex);
        events.push_back(event);
    };
    SpeechService service;
    service.Start(settings, receive);
    service.SetBargeInEnabled(false);
    service.UseVoice(FixtureVoice());
    Check(WaitFor([&] { return saw("Ready", 0); }, std::chrono::seconds(5)),
        "Live Windows speech did not become ready.");

    service.Speak(
        "This is a speech interruption check with a bank sound waiting behind this sentence. "
        "*chuckles* This final sentence should be cancelled.", {}, 71);
    Check(WaitFor([&]
        {
            return saw("Speaking", 71) &&
                SpeechServiceTestAccess::HasPreparedClip(service, clip);
        }, std::chrono::seconds(5)),
        "Live playback never held the bank clip behind the spoken phrase.");

    // Exercise the same public method that the microphone callback invokes.
    // Acoustic trigger detection is deliberately separate from this lifetime check.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto stopStarted = std::chrono::steady_clock::now();
    service.YieldToUser();
    Check(WaitFor([&] { return saw("Stopped", 71); }, std::chrono::seconds(2)),
        "Live Windows speech did not stop after YieldToUser.");
    const auto stopMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stopStarted).count();
    Check(ReadBytes(clip) == original, "Live interruption destroyed the waiting bank clip.");
    Check(!saw("Vocalization", 71), "The cancelled bank clip began playing after interruption.");

    service.Shutdown();
    {
        std::lock_guard lock(eventMutex);
        events.clear();
    }
    SpeechService replay;
    replay.Start(settings, receive);
    replay.SetBargeInEnabled(false);
    replay.UseVoice(FixtureVoice());
    Check(WaitFor([&] { return saw("Ready", 0); }, std::chrono::seconds(5)),
        "The replay service did not become ready.");
    replay.Speak("*chuckles*", {}, 72);
    Check(WaitFor([&] { return saw("VocalizationPlayed", 72); }, std::chrono::seconds(8)),
        "The preserved clip could not play after starting a fresh service.");
    replay.Shutdown();
    Check(ReadBytes(clip) == original && ReadBytes(sourceClip) == original,
        "Live playback changed the fixture or original persistent asset.");
    std::cout << "LIVE Windows SAPI interruption and bank replay passed; stop_ms="
        << stopMilliseconds << ". Microphone-trigger detection and listening quality "
        "are not certified by this check.\n";
#endif
}
