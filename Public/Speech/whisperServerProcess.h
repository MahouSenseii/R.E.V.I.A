#pragma once

#include "Library/structLibrary.h"

#include <mutex>
#include <string>

namespace revia::speech
{

class WhisperServerProcess
{
public:
    WhisperServerProcess() = default;
    ~WhisperServerProcess();

    WhisperServerProcess(const WhisperServerProcess&) = delete;
    WhisperServerProcess& operator=(const WhisperServerProcess&) = delete;

    bool Start(const speechRecognitionSettings& settings, std::string& outError);
    [[nodiscard]] bool IsRunning() const;
    void Stop();

private:
    [[nodiscard]] bool IsRunningLocked() const;
    void StopLocked();

    mutable std::mutex processMutex;
#ifdef _WIN32
    void* processHandle = nullptr;
    void* jobHandle = nullptr;
#endif
};

} // namespace revia::speech
