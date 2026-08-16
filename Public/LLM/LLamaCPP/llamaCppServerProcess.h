#pragma once

#include "Library/structLibrary.h"

#include <string>

class llamaCppServerProcess
{
public:
    llamaCppServerProcess() = default;
    ~llamaCppServerProcess();

    llamaCppServerProcess(const llamaCppServerProcess&) = delete;
    llamaCppServerProcess& operator=(const llamaCppServerProcess&) = delete;

    bool Start(const llmSettings& settings, std::string& outError);
    bool StartEmbedding(const embeddingSettings& settings, std::string& outError);
    bool IsRunning() const;
    bool WasStartedByRevia() const;
    void Stop();

private:
    bool StartInternal(
        const llmSettings& settings,
        bool embeddingMode,
        const std::string& pooling,
        const std::string& device,
        std::string& outError);
#ifdef _WIN32
    void* processHandle = nullptr;
    void* jobHandle = nullptr;
#endif
    bool bShutdownOnExit = true;
};
