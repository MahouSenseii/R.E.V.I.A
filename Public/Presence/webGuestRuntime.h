#pragma once

#include "Library/structLibrary.h"
#include <functional>
#include <memory>
#include <string>

namespace revia::presence {
// An ephemeral conversation-only boundary. No owner context, event bus, memory,
// relationship registry, file IPC or action executor is accepted by this class.
class WebGuestRuntime {
public:
    explicit WebGuestRuntime(const llmSettings& model, std::function<bool()> ownerBusy);
    ~WebGuestRuntime();
    WebGuestRuntime(const WebGuestRuntime&) = delete;
    WebGuestRuntime& operator=(const WebGuestRuntime&) = delete;
    bool Start(int port, const std::string& localToken, bool enabled = false);
    void Stop();
    void Preempt();
    [[nodiscard]] int Port() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
