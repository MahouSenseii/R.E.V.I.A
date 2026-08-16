#pragma once

#include "Library/structLibrary.h"
#include "Speech/qwenTtsServerProcess.h"
#include "Speech/voiceTypes.h"

#include <mutex>
#include <atomic>
#include <string>

namespace revia::speech
{

class QwenTtsClient
{
public:
    QwenTtsClient() = default;
    ~QwenTtsClient();

    QwenTtsClient(const QwenTtsClient&) = delete;
    QwenTtsClient& operator=(const QwenTtsClient&) = delete;

    void Configure(speechSettings settings);
    bool IsAvailable(std::string& outDetail);
    VoiceOperationResult PrepareCloneModel();
    VoiceOperationResult DesignVoice(
        const std::string& text,
        const std::string& description,
        const std::string& language,
        const std::string& outputPath);
    VoiceOperationResult Synthesize(
        const std::string& text,
        const VoicePreset& preset,
        const std::string& outputPath);
    void CancelActiveRequest();
    void Shutdown();

private:
    bool EnsureAvailable(std::string& outError);
    VoiceOperationResult Post(const std::string& endpoint, const std::string& body);

    std::mutex mutex;
    std::mutex processMutex;
    std::atomic<bool> shuttingDown = false;
    speechSettings configuration;
    std::string apiKey;
    QwenTtsServerProcess process;
};

} // namespace revia::speech
