#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace revia::net
{

// A small WebSocket server (RFC 6455), enough for a game that speaks the Neuro SDK
// protocol to connect to her: the upgrade handshake, text frames in both directions,
// fragmented messages reassembled, ping answered, close honoured. Nothing here is
// reachable from the model; it carries JSON that the game server (Games/) reads.
//
// The pieces are exposed on their own so the handshake and the framing can be tested
// against the published vectors rather than only against each other.
[[nodiscard]] std::string Sha1(const std::string& bytes);
[[nodiscard]] std::string Base64Encode(const std::string& bytes);
[[nodiscard]] std::string WebSocketAcceptKey(const std::string& clientKey);

enum class Opcode : std::uint8_t
{
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};

struct Frame
{
    Opcode opcode = Opcode::Text;
    bool fin = true;
    std::string payload;
};

// A frame as bytes. Clients mask; servers do not.
[[nodiscard]] std::string EncodeFrame(Opcode opcode, const std::string& payload, bool mask, bool fin = true);

enum class DecodeResult
{
    Incomplete,
    Frame,
    Error
};

// Takes one frame off the front of `buffer` when a whole one is there.
[[nodiscard]] DecodeResult DecodeFrame(std::string& buffer, Frame& outFrame, std::string& outError,
    std::size_t maximumPayload = 1024 * 1024);

// The client's HTTP upgrade request: its key, and the path it asked for.
[[nodiscard]] bool ParseUpgradeRequest(
    const std::string& request, std::string& outKey, std::string& outPath, std::string& outError);
[[nodiscard]] std::string UpgradeResponse(const std::string& clientKey);

class WebSocketServer
{
public:
    using ConnectionId = std::uint64_t;
    // Called on the connection's own thread. `text` is one whole message.
    using MessageHandler = std::function<void(ConnectionId connection, const std::string& text)>;
    using ConnectionHandler = std::function<void(ConnectionId connection, bool connected)>;

    WebSocketServer() = default;
    ~WebSocketServer();
    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    void SetHandlers(MessageHandler onMessage, ConnectionHandler onConnection);
    // Port 0 picks a free one; Port() says which.
    bool Start(const std::string& host, std::uint16_t port, std::string& outError);
    void Stop();
    [[nodiscard]] bool IsRunning() const { return running.load(); }
    [[nodiscard]] std::uint16_t Port() const { return boundPort; }
    [[nodiscard]] std::size_t ConnectionCount() const;

    bool Send(ConnectionId connection, const std::string& text);
    void Close(ConnectionId connection);

private:
    struct Connection;
    void AcceptLoop();
    void ServeConnection(std::shared_ptr<Connection> connection);
    void Drop(ConnectionId id);

    MessageHandler onMessage;
    ConnectionHandler onConnection;
    std::atomic<bool> running = false;
    std::uint16_t boundPort = 0;
    std::intptr_t listenSocket = -1;
    std::thread acceptor;
    mutable std::mutex mutex;
    std::map<ConnectionId, std::shared_ptr<Connection>> connections;
    ConnectionId nextConnection = 1;
};

} // namespace revia::net
