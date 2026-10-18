#pragma once


#include "Speech/recognitionSettings.h"
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

    // The random prefix every route of the running server sits under ("/<32 hex>"),
    // fresh for each launch. Upstream whisper-server has no authentication, answers any
    // origin, and exposes POST /load; under an unguessable prefix a web page or another
    // local program cannot reach those routes without first learning it. Empty when no
    // server was started.
    [[nodiscard]] std::string RequestPath() const;
    // True only when every process listening on `port` is the one this object started.
    // Windows asks the TCP table for each listener's owning process; elsewhere there is
    // no server to own, so this is false.
    [[nodiscard]] bool OwnsListener(int port) const;

private:
    [[nodiscard]] bool IsRunningLocked() const;
    void StopLocked();

    mutable std::mutex processMutex;
    std::string requestPath;
#ifdef _WIN32
    void* processHandle = nullptr;
    void* jobHandle = nullptr;
#endif
};

} // namespace revia::speech
