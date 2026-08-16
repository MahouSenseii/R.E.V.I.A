#pragma once

#include "Library/structLibrary.h"

#include <string>

namespace revia::speech
{

class QwenTtsServerProcess
{
public:
    QwenTtsServerProcess() = default;
    ~QwenTtsServerProcess();

    QwenTtsServerProcess(const QwenTtsServerProcess&) = delete;
    QwenTtsServerProcess& operator=(const QwenTtsServerProcess&) = delete;

    bool Start(const speechSettings& settings, const std::string& apiKey, std::string& outError);
    bool IsRunning() const;
    bool WasStartedByRevia() const;
    void Stop();

private:
#ifdef _WIN32
    void* processHandle = nullptr;
    void* jobHandle = nullptr;
#endif
};

} // namespace revia::speech
