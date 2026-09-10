#include "testSupport.h"

#include "Performance/performanceRuntime.h"
#include "Performance/songLibrary.h"
#include "Performance/wavAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{

using namespace revia::performance;
using revia::tests::Check;

// Nothing here plays audio. Every test goes through the rehearsal path, which does the
// whole load-and-mix half of a performance and stops before the device is opened -- a
// test suite that filled the room with sound would not get run twice.
//
// The tones below are generated arithmetic, so the suite carries no audio content.

void WriteLittle32(std::ofstream& file, const std::uint32_t value)
{
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(value & 0xFF),
        static_cast<unsigned char>((value >> 8) & 0xFF),
        static_cast<unsigned char>((value >> 16) & 0xFF),
        static_cast<unsigned char>((value >> 24) & 0xFF)};
    file.write(reinterpret_cast<const char*>(bytes), 4);
}

void WriteLittle16(std::ofstream& file, const std::uint16_t value)
{
    const unsigned char bytes[2] = {
        static_cast<unsigned char>(value & 0xFF),
        static_cast<unsigned char>((value >> 8) & 0xFF)};
    file.write(reinterpret_cast<const char*>(bytes), 2);
}

void WriteWav(
    const std::filesystem::path& path,
    const std::vector<std::int16_t>& samples,
    const int sampleRate,
    const int channels)
{
    std::ofstream file(path, std::ios::binary);
    Check(file.is_open(), "Could not create the test WAV at " + path.string());
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    file.write("RIFF", 4);
    WriteLittle32(file, 36 + dataBytes);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    WriteLittle32(file, 16);
    WriteLittle16(file, 1);
    WriteLittle16(file, static_cast<std::uint16_t>(channels));
    WriteLittle32(file, static_cast<std::uint32_t>(sampleRate));
    WriteLittle32(file, static_cast<std::uint32_t>(sampleRate * channels * 2));
    WriteLittle16(file, static_cast<std::uint16_t>(channels * 2));
    WriteLittle16(file, 16);
    file.write("data", 4);
    WriteLittle32(file, dataBytes);
    file.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
    Check(file.good(), "Could not finish writing the test WAV.");
}

// A plain tone. Loud stretches and silent stretches are what the vocal detector reads.
std::vector<std::int16_t> Tone(
    const int sampleRate,
    const int milliseconds,
    const int channels,
    const double amplitude = 0.5,
    const double frequency = 220.0)
{
    const std::int64_t frames = (static_cast<std::int64_t>(sampleRate) * milliseconds) / 1000;
    std::vector<std::int16_t> samples(static_cast<std::size_t>(frames) * channels);
    for (std::int64_t frame = 0; frame < frames; ++frame)
    {
        const double phase =
            2.0 * 3.14159265358979 * frequency * static_cast<double>(frame) / sampleRate;
        const auto value = static_cast<std::int16_t>(
            std::lround(std::sin(phase) * amplitude * 32767.0));
        for (int channel = 0; channel < channels; ++channel)
        {
            samples[static_cast<std::size_t>(frame) * channels + channel] = value;
        }
    }
    return samples;
}

std::vector<std::int16_t> Silence(
    const int sampleRate, const int milliseconds, const int channels)
{
    const std::int64_t frames = (static_cast<std::int64_t>(sampleRate) * milliseconds) / 1000;
    return std::vector<std::int16_t>(static_cast<std::size_t>(frames) * channels, 0);
}

void Append(std::vector<std::int16_t>& target, const std::vector<std::int16_t>& more)
{
    target.insert(target.end(), more.begin(), more.end());
}

struct SongFixture
{
    revia::tests::ScopedTestDirectory directory;
    std::filesystem::path root = directory.root / "Songs";

    SongFixture() { std::filesystem::create_directories(root); }

    std::filesystem::path Make(const std::string& id) const
    {
        const std::filesystem::path folder = root / id;
        std::filesystem::create_directories(folder);
        return folder;
    }

    void Describe(const std::string& id, const nlohmann::json& data) const
    {
        std::ofstream file(root / id / "song.json");
        file << data.dump(2);
        Check(file.good(), "Could not write the test song descriptor.");
    }

    // By pointer because a performance runtime owns a playback thread and a device, so
    // it is deliberately neither copyable nor movable.
    [[nodiscard]] std::unique_ptr<PerformanceRuntime> Runtime(const bool enabled = true) const
    {
        auto runtime = std::make_unique<PerformanceRuntime>();
        PerformanceConfig config;
        config.enabled = enabled;
        // Absolute, so the runtime path resolver leaves it alone and the test stays
        // inside its own temporary directory.
        config.songLibraryPath = root.string();
        runtime->Configure(config);
        return runtime;
    }
};

void TestWavRoundTrip()
{
    revia::tests::ScopedTestDirectory directory;
    const std::filesystem::path path = directory.root / "tone.wav";
    WriteWav(path, Tone(22050, 500, 2), 22050, 2);

    PcmAudio audio;
    std::string error;
    Check(ReadWavFile(path, audio, error), "A plain 16-bit stereo WAV did not read: " + error);
    Check(audio.sampleRate == 22050 && audio.channels == 2, "The WAV format was misread.");
    Check(std::llabs(audio.DurationMs() - 500) <= 2,
        "The WAV duration was wrong: " + std::to_string(audio.DurationMs()));
}

void TestUnreadableAudioIsRefusedByName()
{
    revia::tests::ScopedTestDirectory directory;
    PcmAudio audio;
    std::string error;

    const std::filesystem::path missing = directory.root / "absent.wav";
    Check(!ReadWavFile(missing, audio, error) && !error.empty(),
        "A missing file was not refused.");

    const std::filesystem::path notRiff = directory.root / "notaudio.wav";
    {
        std::ofstream file(notRiff, std::ios::binary);
        file << "this is not a wave file at all, not even close";
    }
    Check(!ReadWavFile(notRiff, audio, error) &&
        error.find("RIFF/WAVE") != std::string::npos,
        "A non-RIFF file was not refused by name.");

    // A header that promises more audio than the file holds is common enough that
    // refusing it would be unhelpful; what is really there should still play.
    const std::filesystem::path truncated = directory.root / "short.wav";
    WriteWav(truncated, Tone(22050, 200, 1), 22050, 1);
    {
        const auto size = std::filesystem::file_size(truncated);
        std::filesystem::resize_file(truncated, size - 2000);
    }
    Check(ReadWavFile(truncated, audio, error) && audio.DurationMs() > 0,
        "A truncated file lost the audio it did contain: " + error);
}

void TestMonoBecomesStereoAndMixSaturates()
{
    PcmAudio mono;
    mono.sampleRate = 8000;
    mono.channels = 1;
    mono.samples = {100, -100, 200};
    std::string error;
    Check(ConvertToStereo(mono, error) && mono.channels == 2 &&
        mono.samples.size() == 6 && mono.samples[0] == 100 && mono.samples[1] == 100,
        "A mono track did not duplicate across both channels.");

    // Two loud tracks summed must saturate rather than wrap: wrapping turns a loud
    // moment into a burst of noise, which is the worst possible failure to be quiet about.
    std::vector<std::int16_t> base = {30000, -30000, 10};
    const std::vector<std::int16_t> overlay = {30000, -30000, 10};
    const std::size_t clipped = MixInto(base, overlay, 1.0, 1.0);
    Check(base[0] == 32767 && base[1] == -32768 && base[2] == 20,
        "Mixing wrapped instead of saturating.");
    Check(clipped == 2, "The clipped-sample count was wrong: " + std::to_string(clipped));

    // A shorter overlay must not truncate the song it is mixed into.
    std::vector<std::int16_t> longBase = {1, 2, 3, 4};
    Check(MixInto(longBase, {1}, 1.0, 1.0) == 0 && longBase.size() == 4 &&
        longBase[3] == 4, "Mixing a short overlay damaged the rest of the track.");
}

void TestVocalSpansComeFromTheAudio()
{
    // Sung, silent, sung. A mouth that opens because a file said so is eventually
    // wrong; this reads the samples instead.
    PcmAudio vocal;
    vocal.sampleRate = 16000;
    vocal.channels = 1;
    Append(vocal.samples, Tone(16000, 600, 1, 0.6));
    Append(vocal.samples, Silence(16000, 900, 1));
    Append(vocal.samples, Tone(16000, 600, 1, 0.6));

    const std::vector<VocalSpan> spans = DetectVocalSpans(vocal);
    Check(spans.size() == 2,
        "Two sung phrases were not detected: got " + std::to_string(spans.size()));
    Check(spans[0].startMs < 100 && spans[0].endMs > 400,
        "The first sung phrase had the wrong bounds.");
    Check(spans[1].startMs > 1200, "The second sung phrase started too early.");

    // A breath between two words is not the end of a phrase.
    PcmAudio breath;
    breath.sampleRate = 16000;
    breath.channels = 1;
    Append(breath.samples, Tone(16000, 400, 1, 0.6));
    Append(breath.samples, Silence(16000, 120, 1));
    Append(breath.samples, Tone(16000, 400, 1, 0.6));
    Check(DetectVocalSpans(breath).size() == 1,
        "A short breath split one phrase into two.");

    PcmAudio quiet;
    quiet.sampleRate = 16000;
    quiet.channels = 1;
    quiet.samples = Silence(16000, 500, 1);
    Check(DetectVocalSpans(quiet).empty(), "Silence was reported as singing.");
}

void TestLibraryFindsSongsAndRefusesEscapes()
{
    SongFixture fixture;
    // The zero-configuration shape: one folder, one wav, nothing else.
    const auto lone = fixture.Make("hummed");
    WriteWav(lone / "whatever-i-called-it.wav", Tone(22050, 300, 1), 22050, 1);

    // The karaoke shape: two named tracks plus a descriptor.
    const auto full = fixture.Make("duet");
    WriteWav(full / "instrumental.wav", Tone(22050, 800, 2, 0.3), 22050, 2);
    WriteWav(full / "vocal.wav", Tone(22050, 800, 1, 0.5), 22050, 1);
    fixture.Describe("duet", {
        {"title", "Duet Practice"},
        {"artist", "Test Fixture"},
        {"sections", {{{"startMs", 0}, {"label", "Intro"}, {"line", "first marked line"}}}}});

    // A folder with nothing playable must say so rather than vanish.
    fixture.Make("empty-folder");

    SongLibrary library;
    library.SetRoot(fixture.root);
    const std::vector<SongSummary> songs = library.List();
    Check(songs.size() == 3, "The library did not list every folder.");

    const auto find = [&](const std::string& id)
    {
        return std::find_if(songs.begin(), songs.end(),
            [&id](const SongSummary& song) { return song.id == id; });
    };
    Check(find("hummed")->usable && find("hummed")->hasVocal &&
        !find("hummed")->hasInstrumental,
        "A folder holding one loose wav was not usable as a song.");
    Check(find("duet")->usable && find("duet")->title == "Duet Practice" &&
        find("duet")->hasInstrumental && find("duet")->hasVocal,
        "The two-track song did not load its descriptor.");
    Check(!find("empty-folder")->usable && !find("empty-folder")->problem.empty(),
        "A folder with no audio did not report why it is unusable.");

    // A song id arrives from a chat message, so it is untrusted text.
    const std::vector<std::string> unsafeIds = {
        "..", "../secrets", "a\\b", "a/b", "", std::string(80, 'x')};
    for (const std::string& unsafe : unsafeIds)
    {
        Check(!SongLibrary::IsSafeSongId(unsafe),
            "An unsafe song id was accepted: '" + unsafe + "'");
        SongAsset asset;
        std::string error;
        Check(!library.Load(unsafe, asset, error), "An unsafe song id reached the filesystem.");
    }

    std::string resolved;
    std::string error;
    Check(library.Resolve("duet", resolved, error) && resolved == "duet",
        "An exact song id did not resolve.");
    Check(library.Resolve("Duet Pr", resolved, error) && resolved == "duet",
        "A title prefix did not resolve.");
    Check(!library.Resolve("nothing-like-this", resolved, error) && !error.empty(),
        "A missing song resolved to something.");

    // Two candidates must be reported, not guessed between.
    const auto sibling = fixture.Make("duet-two");
    WriteWav(sibling / "vocal.wav", Tone(22050, 200, 1), 22050, 1);
    Check(!library.Resolve("duet", resolved, error) || resolved == "duet",
        "An exact match stopped winning over a prefix match.");
    Check(!library.Resolve("due", resolved, error) &&
        error.find("several songs") != std::string::npos,
        "An ambiguous name was resolved instead of reported.");
}

void TestRehearsalMixesWithoutPlaying()
{
    SongFixture fixture;
    const auto folder = fixture.Make("karaoke");
    WriteWav(folder / "instrumental.wav", Tone(22050, 1000, 2, 0.4), 22050, 2);
    std::vector<std::int16_t> vocal;
    Append(vocal, Silence(22050, 200, 1));
    Append(vocal, Tone(22050, 500, 1, 0.7));
    Append(vocal, Silence(22050, 300, 1));
    WriteWav(folder / "vocal.wav", vocal, 22050, 1);
    fixture.Describe("karaoke", {
        {"title", "Practice Take"},
        {"sections", {
            {{"startMs", 0}, {"label", "Intro"}},
            {{"startMs", 200}, {"label", "Verse"}, {"line", "the marked line"}}}}});

    const auto runtime = fixture.Runtime();
    const SongRehearsal rehearsal = runtime->Rehearse("karaoke");
    Check(rehearsal.succeeded, "A well-formed song did not rehearse: " + rehearsal.error);
    Check(rehearsal.title == "Practice Take" && rehearsal.sampleRate == 22050,
        "The rehearsal reported the wrong song details.");
    Check(std::llabs(rehearsal.durationMs - 1000) <= 5,
        "The mixed duration was wrong: " + std::to_string(rehearsal.durationMs));
    Check(rehearsal.hasInstrumental && rehearsal.hasVocal && rehearsal.sectionCount == 2,
        "The rehearsal lost a track or a section.");
    Check(rehearsal.vocalSpanCount == 1,
        "The sung phrase in the vocal track was not found.");
    // Rehearsing must not have started anything.
    Check(!runtime->IsPerforming() &&
        runtime->Status().state == PerformanceState::Idle,
        "A rehearsal left the runtime performing.");
}

void TestUnplayableSongsFailWithAReason()
{
    SongFixture fixture;

    // Two tracks at different rates would need resampling, and a hasty resampler sounds
    // worse than a refusal that says exactly what to fix.
    const auto mismatch = fixture.Make("mismatched");
    WriteWav(mismatch / "instrumental.wav", Tone(44100, 300, 2), 44100, 2);
    WriteWav(mismatch / "vocal.wav", Tone(22050, 300, 1), 22050, 1);

    const auto tooLong = fixture.Make("marathon");
    WriteWav(tooLong / "vocal.wav", Tone(8000, 3000, 1), 8000, 1);

    const auto broken = fixture.Make("broken");
    {
        std::ofstream file(broken / "vocal.wav", std::ios::binary);
        file << "RIFFxxxxWAVEnope";
    }

    const auto runtime = fixture.Runtime();
    const SongRehearsal mismatched = runtime->Rehearse("mismatched");
    Check(!mismatched.succeeded &&
        mismatched.error.find("different sample rates") != std::string::npos,
        "Mismatched sample rates were not reported clearly.");

    PerformanceConfig shortLimit;
    shortLimit.songLibraryPath = fixture.root.string();
    shortLimit.maximumSongSeconds = 1;
    PerformanceRuntime limited;
    limited.Configure(shortLimit);
    const SongRehearsal marathon = limited.Rehearse("marathon");
    Check(!marathon.succeeded && marathon.error.find("longer than") != std::string::npos,
        "A song past the configured length limit was accepted.");

    const SongRehearsal corrupt = runtime->Rehearse("broken");
    Check(!corrupt.succeeded && !corrupt.error.empty(),
        "A corrupt WAV did not fail with a reason.");
    Check(runtime->Rehearse("no-such-song").succeeded == false,
        "A missing song rehearsed successfully.");
}

void TestSingingStaysOffAndStaysContained()
{
    SongFixture fixture;
    const auto folder = fixture.Make("quiet");
    WriteWav(folder / "vocal.wav", Tone(22050, 200, 1), 22050, 1);

    const auto disabled = fixture.Runtime(false);
    std::string error;
    Check(!disabled->Start("quiet", error) &&
        error.find("turned off") != std::string::npos,
        "A disabled performance runtime still started a song.");
    Check(!disabled->IsPerforming(), "A refused start left the runtime performing.");

    // The failure path must not need an audio device, a song, or a thread that outlives
    // the call: a broken song is a typed result, never an exception.
    const auto runtime = fixture.Runtime();
    Check(!runtime->Start("does-not-exist", error) && !error.empty(),
        "An unknown song did not fail with a reason.");
    Check(!runtime->Start("", error), "An empty song name was accepted.");

    // Stop on an idle runtime is a no-op rather than an error, because the stop path has
    // to be safe to call from shutdown without checking anything first.
    runtime->Stop("nothing is playing");
    Check(!runtime->IsPerforming(), "Stopping an idle runtime started something.");
}

} // namespace

void RunPerformanceTests()
{
    TestWavRoundTrip();
    TestUnreadableAudioIsRefusedByName();
    TestMonoBecomesStereoAndMixSaturates();
    TestVocalSpansComeFromTheAudio();
    TestLibraryFindsSongsAndRefusesEscapes();
    TestRehearsalMixesWithoutPlaying();
    TestUnplayableSongsFailWithAReason();
    TestSingingStaysOffAndStaysContained();
    std::cout << "Performance tests passed: songs load, mix, and refuse with reasons.\n";
}
