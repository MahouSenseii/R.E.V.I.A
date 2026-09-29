#pragma once

#include "Net/tcpStream.h"
#include "Stage/stageProtocol.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace revia::stage
{

// The host's end of the channel: one connection to the guest's action runtime, one
// request in flight at a time, every answer matched to its id.
class StageChannelClient
{
public:
    StageChannelClient() = default;
    ~StageChannelClient();
    StageChannelClient(const StageChannelClient&) = delete;
    StageChannelClient& operator=(const StageChannelClient&) = delete;

    // Connects and reads the guest's hello. False with the reason otherwise.
    bool Connect(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout, std::string& outError);
    [[nodiscard]] bool IsConnected() const;
    [[nodiscard]] GuestInfo Guest() const;
    void Disconnect();

    // Performs one request in the guest. A result that does not come in time, a
    // refusal, or a lost connection comes back as a result that was not attempted,
    // with the reason in its message.
    [[nodiscard]] actions::ActionResult Execute(
        const actions::ActionRequest& request, StageTier tier, std::chrono::milliseconds timeout);
    // Tells the guest to stop performing anything further until restarted.
    bool Halt();
    bool Ping(std::chrono::milliseconds timeout);

private:
    mutable std::mutex mutex;
    std::optional<net::TcpStream> stream;
    GuestInfo guest;
    std::uint64_t nextId = 1;
};

// The guest's end: serves one host at a time, performs requests through the executor
// it was given, refuses anything above the tier it was started with, and stops for
// good on a halt.
class StageGuestServer
{
public:
    using Executor = std::function<actions::ActionResult(const actions::ActionRequest&, StageTier)>;

    StageGuestServer() = default;
    ~StageGuestServer();
    StageGuestServer(const StageGuestServer&) = delete;
    StageGuestServer& operator=(const StageGuestServer&) = delete;

    bool Start(const std::string& host, std::uint16_t port, GuestInfo self, Executor executor, std::string& outError);
    void Stop();
    [[nodiscard]] bool IsRunning() const { return running.load(); }
    [[nodiscard]] std::uint16_t Port() const { return listener.Port(); }
    [[nodiscard]] bool Halted() const { return halted.load(); }
    [[nodiscard]] std::size_t Served() const { return served.load(); }

private:
    void Serve();

    net::TcpListener listener;
    GuestInfo self;
    Executor executor;
    std::atomic<bool> running = false;
    std::atomic<bool> halted = false;
    std::atomic<std::size_t> served = 0;
    std::thread worker;
};

} // namespace revia::stage
