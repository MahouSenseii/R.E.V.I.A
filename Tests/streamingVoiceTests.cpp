#include "testSupport.h"

#include "Library/structLibrary.h"
#include "Speech/pcmStream.h"
#include "Speech/qwenTtsClient.h"
#include "Speech/voiceTypes.h"

#include <chrono>
#include <cstdint>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The first phrase of a reply as a stream of PCM, not one WAV.
//
// Whatever the worker can do -- today it hands the whole waveform over after
// generation, and one day it decodes as it generates -- the client side has to take
// audio in pieces, carry a byte the transport cut in half, measure the first piece
// where it lands, and fall back to the WAV path on a worker that has no stream.
namespace
{
using namespace std::chrono_literals;
using revia::speech::PcmStreamAssembler;
using revia::speech::QwenTtsClient;
using revia::speech::VoiceOperationResult;
using revia::speech::VoicePreset;
using revia::speech::WrapPcmAsWav;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

void TestChunksAssembleIntoSamplesWithAByteCarried()
{
    PcmStreamAssembler assembler;
    const std::uint8_t first[] = {0x01, 0x00, 0x02, 0x00, 0x03};
    const std::uint8_t second[] = {0x00, 0xFF, 0xFF};
    assembler.Append(first, sizeof(first));
    Check(assembler.SampleCount() == 2 && assembler.HasPendingByte(),
        "A half sample at the end of a chunk was not carried.");
    assembler.Append(second, sizeof(second));
    Check(assembler.SampleCount() == 4 && !assembler.HasPendingByte() &&
        assembler.Samples()[2] == 3 && assembler.Samples()[3] == -1,
        "The carried byte did not complete the sample it belonged to.");
    Check(assembler.BufferedMilliseconds(4000) == 1.0,
        "Buffered milliseconds were not samples over the rate.");
    const std::vector<std::uint8_t> wav = assembler.ToWav(24000);
    Check(wav.size() == 44 + 8 && std::string(wav.begin(), wav.begin() + 4) == "RIFF" &&
        std::string(wav.begin() + 8, wav.begin() + 12) == "WAVE" &&
        wav[22] == 1 && wav[24] == 0xC0 && wav[25] == 0x5D && wav[34] == 16 &&
        wav[40] == 8 && wav[44] == 0x01 && wav[50] == 0xFF && wav[51] == 0xFF,
        "The WAV wrapper did not describe 16-bit mono at the sample rate given.");
    Check(WrapPcmAsWav({}, 0).size() == 44, "An empty stream did not wrap as a header alone.");
}

// A worker that streams three pieces with a pause, or refuses the endpoint entirely.
class StreamingWorker
{
public:
    explicit StreamingWorker(const bool hasStream)
    {
        server.Get("/health", [](const auto&, auto& response)
        {
            response.set_content(R"({"status":"ok","model_kind":"clone","device":"cpu"})",
                "application/json");
        });
        if (hasStream)
        {
            server.Post("/v1/audio/pcm-stream", [this](const auto& request, auto& response)
            {
                {
                    std::lock_guard lock(mutex);
                    lastBody = request.body;
                }
                response.set_header("X-Revia-Sample-Rate", "8000");
                response.set_header("X-Revia-Streaming", "whole");
                response.set_header("X-Revia-Elapsed-Ms", "40.5");
                response.set_header("X-Revia-Device", "cpu");
                response.set_chunked_content_provider("audio/pcm",
                    [](std::size_t, httplib::DataSink& sink)
                    {
                        const std::uint8_t one[] = {0x10, 0x00, 0x20, 0x00, 0x30};
                        const std::uint8_t two[] = {0x00, 0x40, 0x00};
                        const std::uint8_t three[] = {0x50, 0x00};
                        sink.write(reinterpret_cast<const char*>(one), sizeof(one));
                        std::this_thread::sleep_for(60ms);
                        sink.write(reinterpret_cast<const char*>(two), sizeof(two));
                        sink.write(reinterpret_cast<const char*>(three), sizeof(three));
                        sink.done();
                        return true;
                    });
            });
        }
        server.Post("/v1/audio/pcm", [](const auto&, auto& response)
        {
            const std::vector<std::uint8_t> wav = WrapPcmAsWav({7, 8, 9}, 8000);
            response.set_header("X-Revia-Sample-Rate", "8000");
            response.set_content(std::string(wav.begin(), wav.end()), "audio/wav");
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the streaming voice fixture.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        Check(server.is_running(), "The streaming voice fixture did not start.");
    }
    ~StreamingWorker() { server.stop(); thread.join(); }
    std::string LastBody() { std::lock_guard lock(mutex); return lastBody; }
    int port = 0;
private:
    httplib::Server server;
    std::mutex mutex;
    std::string lastBody;
    std::jthread thread;
};

VoicePreset Preset(const ScopedTestDirectory& directory)
{
    VoicePreset preset;
    preset.id = "fixture";
    preset.name = "Fixture";
    preset.language = "English";
    preset.referenceText = "Reference.";
    preset.referenceAudioPath = (directory.root / "reference.wav").string();
    revia::tests::WriteMinimalWav(directory.root / "reference.wav");
    return preset;
}

speechSettings Settings(const int port)
{
    speechSettings settings;
    settings.qwenHost = "127.0.0.1";
    settings.qwenPort = port;
    settings.qwenRequestTimeoutSeconds = 10;
    return settings;
}

void TestAStreamedPhraseArrivesInPiecesAndBecomesTheSameWav()
{
    ScopedTestDirectory directory;
    StreamingWorker worker(true);
    QwenTtsClient client;
    client.Configure(Settings(worker.port));
    std::vector<std::size_t> pieces;
    const VoiceOperationResult result = client.SynthesizePcmStream("Hello there.", Preset(directory),
        [&pieces](const std::uint8_t*, const std::size_t count) { pieces.push_back(count); return true; });
    Check(result.succeeded, "The streamed phrase failed: " + result.message);
    Check(result.streamedChunks >= 2 && pieces.size() == static_cast<std::size_t>(result.streamedChunks),
        "The audio did not arrive in pieces: " + std::to_string(result.streamedChunks));
    Check(result.firstChunkMilliseconds >= 0.0 && result.firstChunkMilliseconds < result.cppResponseMilliseconds,
        "The first piece was not timed before the whole response.");
    Check(result.sampleRate == 8000 && !result.incrementalAudio && result.elapsedMilliseconds > 40.0,
        "The stream's headers were not read.");
    const std::vector<std::uint8_t> expected = WrapPcmAsWav({0x10, 0x20, 0x30, 0x40, 0x50}, 8000);
    Check(result.audioBytes == expected,
        "The assembled WAV differs from the samples the worker sent (" +
        std::to_string(result.audioBytes.size()) + " bytes).");
    Check(result.audioDurationMilliseconds > 0.6 && result.audioDurationMilliseconds < 0.7,
        "The duration was not derived from the samples when the worker gave none.");
    Check(worker.LastBody().find("\"text\":\"Hello there.\"") != std::string::npos,
        "The phrase was not sent to the worker.");

    // A caller that stops listening ends the stream as a failure, never as audio.
    const VoiceOperationResult abandoned = client.SynthesizePcmStream("Hello there.", Preset(directory),
        [](const std::uint8_t*, std::size_t) { return false; });
    Check(!abandoned.succeeded && abandoned.message.find("abandoned") != std::string::npos,
        "An abandoned stream was reported as audio.");
}

void TestAWorkerWithoutTheStreamRefusesSoTheCallerFallsBack()
{
    ScopedTestDirectory directory;
    StreamingWorker worker(false);
    QwenTtsClient client;
    client.Configure(Settings(worker.port));
    const VoiceOperationResult streamed = client.SynthesizePcmStream("Hello.", Preset(directory));
    Check(!streamed.succeeded && streamed.audioBytes.empty() &&
        streamed.message.find("404") != std::string::npos,
        "A worker without the endpoint did not refuse the stream cleanly: " + streamed.message);
    const VoiceOperationResult whole = client.SynthesizePcm("Hello.", Preset(directory));
    Check(whole.succeeded && whole.audioBytes == WrapPcmAsWav({7, 8, 9}, 8000),
        "The WAV path was not still available on the same worker.");
}
} // namespace

void RunStreamingVoiceTests()
{
    TestChunksAssembleIntoSamplesWithAByteCarried();
    TestAStreamedPhraseArrivesInPiecesAndBecomesTheSameWav();
    TestAWorkerWithoutTheStreamRefusesSoTheCallerFallsBack();
    std::cout << "A streamed phrase arrives in pieces with a split sample carried, is timed "
                 "at its first piece, becomes the same WAV, and a worker without the stream "
                 "is fallen back from.\n";
}
