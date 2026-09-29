#pragma once

#include "Net/webSocket.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::games
{

// Games that speak the Neuro SDK protocol (github.com/VedalAI/neuro-sdk), which is
// how a game lets an AI play it without pixels or keystrokes: the game connects to
// her over a WebSocket, registers the actions it accepts with JSON schemas, sends
// context in plain words, and can force her to pick one of them now. She answers
// with an action and its data; the game says whether it worked. Nothing in this
// protocol reaches the machine -- an action is JSON to the game and nothing else --
// which is exactly why it is the right way for her to play.
struct GameAction
{
    std::string name;
    std::string description;
    // A JSON Schema of type object, as the game sent it; "{}" when it sent none.
    std::string schema = "{}";
};

struct ContextEntry
{
    std::string message;
    // Sent for her to know, not to react to.
    bool silent = false;
    std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
};

struct ForceRequest
{
    std::uint64_t serial = 0;
    std::string query;
    std::string state;
    std::string priority;
    bool ephemeralContext = false;
    // Which registered actions she may pick from.
    std::vector<std::string> actionNames;
    std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
};

struct GameSnapshot
{
    std::string name;
    std::uint64_t connection = 0;
    std::vector<GameAction> actions;
    std::vector<ContextEntry> context;
    std::optional<ForceRequest> force;
};

struct GameEvent
{
    enum class Kind
    {
        Connected,
        Disconnected,
        Context,
        ActionsChanged,
        Force,
        Result
    };
    Kind kind = Kind::Context;
    std::string game;
    std::string text;
    bool silent = false;
    std::uint64_t serial = 0;
};

struct ActionResultMessage
{
    std::string id;
    bool success = false;
    std::string message;
};

class NeuroGameServer
{
public:
    // Called on the connection's thread; must not block on the server.
    using EventHandler = std::function<void(const GameEvent&)>;
    static constexpr std::size_t MaximumContext = 40;
    static constexpr std::size_t MaximumActions = 64;

    NeuroGameServer();
    ~NeuroGameServer();
    NeuroGameServer(const NeuroGameServer&) = delete;
    NeuroGameServer& operator=(const NeuroGameServer&) = delete;

    void SetEventHandler(EventHandler handler);
    bool Start(const std::string& host, std::uint16_t port, std::string& outError);
    void Stop();
    [[nodiscard]] bool IsRunning() const { return server.IsRunning(); }
    [[nodiscard]] std::uint16_t Port() const { return server.Port(); }

    [[nodiscard]] std::vector<GameSnapshot> Games() const;
    [[nodiscard]] std::optional<GameSnapshot> Game(const std::string& name) const;

    // Sends one action the game registered. Returns its id, or empty with the reason.
    std::string SendAction(
        const std::string& game, const std::string& name, const std::string& dataJson,
        std::string& outError);
    // The game's verdict on an action she sent, once it arrives.
    [[nodiscard]] std::optional<ActionResultMessage> WaitForResult(
        const std::string& actionId, std::chrono::milliseconds timeout);
    // A force answered (or given up on) is no longer pending.
    void ClearForce(const std::string& game, std::uint64_t serial);
    bool SendSpeechFinished(const std::string& game, bool isFinal = true);
    void Disconnect(const std::string& game);

    // The protocol's rules, on their own.
    [[nodiscard]] static bool ValidActionName(const std::string& name);
    // A small JSON Schema: type, properties, required, additionalProperties, enum,
    // const, minimum, maximum, minLength, maxLength, items, minItems, maxItems.
    [[nodiscard]] static bool ValidateAgainstSchema(
        const std::string& dataJson, const std::string& schemaJson, std::string& outError);
    // The least object the schema accepts: what she sends when no brain can choose.
    [[nodiscard]] static std::string MinimalDataFor(const std::string& schemaJson);

private:
    struct GameState
    {
        std::string name;
        std::uint64_t connection = 0;
        std::vector<GameAction> actions;
        std::vector<ContextEntry> context;
        std::optional<ForceRequest> force;
    };

    void OnMessage(net::WebSocketServer::ConnectionId connection, const std::string& text);
    void OnConnection(net::WebSocketServer::ConnectionId connection, bool connected);
    void Emit(const GameEvent& event);
    [[nodiscard]] static GameSnapshot Snapshot(const GameState& state);

    net::WebSocketServer server;
    EventHandler handler;
    mutable std::mutex mutex;
    std::map<net::WebSocketServer::ConnectionId, GameState> games;
    std::map<std::string, ActionResultMessage> results;
    std::condition_variable resultsReady;
    std::uint64_t nextActionId = 1;
    std::uint64_t nextForceSerial = 1;
};

} // namespace revia::games
