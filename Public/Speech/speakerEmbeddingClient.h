#pragma once

#include "Core/pythonWorkerProcess.h"
#include "Library/structLibrary.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::speech
{

// One utterance to the speaker worker, one unit embedding back.
//
// The worker (Tools/speaker_id_service.py) is started on first use with a key made
// for this session, exactly as the voice worker is, and stopped with the service. A
// worker that is not there is an ordinary outcome: the transcript goes on without an
// embedding, and the session simply cannot say who spoke.
class SpeakerEmbeddingClient
{
public:
    SpeakerEmbeddingClient() = default;
    ~SpeakerEmbeddingClient();

    SpeakerEmbeddingClient(const SpeakerEmbeddingClient&) = delete;
    SpeakerEmbeddingClient& operator=(const SpeakerEmbeddingClient&) = delete;

    void Configure(const speechRecognitionSettings& settings);
    // A worker already running somewhere -- a test's fake -- with its key. Nothing is
    // launched after this.
    void UseEndpoint(std::string host, int port, std::string token);
    [[nodiscard]] bool Enabled() const;

    // The embedding of the utterance in the WAV, or nothing with the reason.
    [[nodiscard]] std::optional<std::vector<float>> Embed(
        const std::filesystem::path& wavePath, std::string& outError);
    [[nodiscard]] bool IsAvailable(std::string& outDetail);
    void Shutdown();

private:
    [[nodiscard]] bool EnsureAvailable(std::string& outError);

    std::mutex mutex;
    speechRecognitionSettings configuration;
    std::string host = "127.0.0.1";
    int port = 0;
    std::string token;
    bool external = false;
    bool closed = false;
    core::PythonWorkerProcess process;
};

} // namespace revia::speech
