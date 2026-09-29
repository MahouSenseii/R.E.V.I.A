#include "Stage/stageChannel.h"

#include <utility>

namespace revia::stage
{

namespace
{
constexpr auto HelloTimeout = std::chrono::milliseconds(5000);
constexpr auto ReadSlice = std::chrono::milliseconds(100);

actions::ActionResult NotAttempted(const std::string& why)
{
    actions::ActionResult result;
    result.attempted = false;
    result.succeeded = false;
    result.message = why;
    result.backend = "stage";
    return result;
}
} // namespace

StageChannelClient::~StageChannelClient()
{
    Disconnect();
}

bool StageChannelClient::Connect(
    const std::string& host, const std::uint16_t port, const std::chrono::milliseconds timeout, std::string& outError)
{
    std::lock_guard lock(mutex);
    stream.reset();
    std::optional<net::TcpStream> connection = net::TcpStream::Connect(host, port, timeout, outError);
    if (!connection) return false;
    std::string line;
    bool closed = false;
    if (!connection->ReadLine(line, HelloTimeout, closed))
    {
        outError = closed ? "The stage closed the connection before saying hello."
                          : "The stage did not say hello in time.";
        return false;
    }
    GuestInfo said;
    if (!ReadHello(line, said, outError)) return false;
    guest = said;
    stream = std::move(connection);
    return true;
}

bool StageChannelClient::IsConnected() const
{
    std::lock_guard lock(mutex);
    return stream && stream->IsOpen();
}

GuestInfo StageChannelClient::Guest() const
{
    std::lock_guard lock(mutex);
    return guest;
}

void StageChannelClient::Disconnect()
{
    std::lock_guard lock(mutex);
    stream.reset();
}

actions::ActionResult StageChannelClient::Execute(
    const actions::ActionRequest& request, const StageTier tier, const std::chrono::milliseconds timeout)
{
    std::lock_guard lock(mutex);
    if (!stream || !stream->IsOpen()) return NotAttempted("The stage is not connected.");
    const std::string id = "stage-" + std::to_string(nextId++);
    if (!stream->WriteLine(EncodeRequest(id, request, tier)))
    {
        stream.reset();
        return NotAttempted("The stage connection was lost while sending the request.");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::string line;
        bool closed = false;
        if (!stream->ReadLine(line, ReadSlice, closed))
        {
            if (closed)
            {
                stream.reset();
                return NotAttempted("The stage closed the connection before answering.");
            }
            continue;
        }
        Envelope envelope;
        std::string error;
        if (!Decode(line, envelope, error)) continue;
        if (envelope.type == "pong" || envelope.type == "hello") continue;
        if (envelope.id != id) continue;
        if (envelope.type == "refused")
        {
            std::string reason;
            (void)ReadRefusal(line, reason);
            actions::ActionResult refused = NotAttempted("The stage refused it: " + reason);
            refused.backend = "stage:" + guest.name;
            return refused;
        }
        if (envelope.type == "result")
        {
            actions::ActionResult result;
            if (!ReadResult(line, result, error)) return NotAttempted("The stage's answer was not readable: " + error);
            if (result.backend.empty()) result.backend = "stage:" + guest.name;
            return result;
        }
    }
    return NotAttempted("The stage did not answer in time; the request may still be running there.");
}

bool StageChannelClient::Halt()
{
    std::lock_guard lock(mutex);
    if (!stream || !stream->IsOpen()) return false;
    return stream->WriteLine(EncodeHalt());
}

bool StageChannelClient::Ping(const std::chrono::milliseconds timeout)
{
    std::lock_guard lock(mutex);
    if (!stream || !stream->IsOpen() || !stream->WriteLine(EncodePing())) return false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::string line;
        bool closed = false;
        if (!stream->ReadLine(line, ReadSlice, closed))
        {
            if (closed)
            {
                stream.reset();
                return false;
            }
            continue;
        }
        Envelope envelope;
        std::string error;
        if (Decode(line, envelope, error) && envelope.type == "pong") return true;
    }
    return false;
}

StageGuestServer::~StageGuestServer()
{
    Stop();
}

bool StageGuestServer::Start(
    const std::string& host, const std::uint16_t port, GuestInfo inputSelf, Executor inputExecutor, std::string& outError)
{
    Stop();
    if (!inputExecutor)
    {
        outError = "The guest needs an executor to perform requests with.";
        return false;
    }
    if (!listener.Start(host, port, outError)) return false;
    self = std::move(inputSelf);
    executor = std::move(inputExecutor);
    halted.store(false);
    running.store(true);
    worker = std::thread([this] { Serve(); });
    return true;
}

void StageGuestServer::Stop()
{
    if (!running.exchange(false))
    {
        if (worker.joinable()) worker.join();
        return;
    }
    listener.Stop();
    if (worker.joinable()) worker.join();
}

void StageGuestServer::Serve()
{
    while (running.load())
    {
        std::optional<net::TcpStream> connection = listener.Accept(std::chrono::milliseconds(200));
        if (!connection) continue;
        if (!connection->WriteLine(EncodeHello(self))) continue;
        while (running.load() && connection->IsOpen())
        {
            std::string line;
            bool closed = false;
            if (!connection->ReadLine(line, ReadSlice, closed))
            {
                if (closed) break;
                continue;
            }
            Envelope envelope;
            std::string error;
            if (!Decode(line, envelope, error)) continue;
            if (envelope.type == "ping")
            {
                (void)connection->WriteLine(EncodePong());
                continue;
            }
            if (envelope.type == "halt")
            {
                // Final until the guest is restarted: a halt is the host's hand on the
                // switch, not a pause, and nothing should quietly resume after it.
                halted.store(true);
                continue;
            }
            if (envelope.type != "request") continue;
            actions::ActionRequest request;
            StageTier tier = StageTier::System;
            if (!ReadRequest(line, request, tier, error))
            {
                (void)connection->WriteLine(EncodeRefusal(envelope.id, "the request was not readable: " + error));
                continue;
            }
            if (halted.load())
            {
                (void)connection->WriteLine(EncodeRefusal(envelope.id, "the stage was halted; restart the guest to continue"));
                continue;
            }
            // The guest's own judgement of the tier, not the host's word for it.
            const StageTier needed = TierFor(request);
            const StageTier asked = tier > needed ? tier : needed;
            if (static_cast<int>(asked) > static_cast<int>(self.grantedTier))
            {
                (void)connection->WriteLine(EncodeRefusal(envelope.id,
                    std::string("this needs ") + ToString(asked) + " and the guest was started with " +
                        ToString(self.grantedTier)));
                continue;
            }
            actions::ActionResult result;
            try
            {
                result = executor(request, asked);
            }
            catch (const std::exception& failure)
            {
                result.attempted = true;
                result.message = std::string("The guest's executor failed: ") + failure.what();
            }
            served.fetch_add(1);
            (void)connection->WriteLine(EncodeResult(envelope.id, result));
        }
    }
}

} // namespace revia::stage
