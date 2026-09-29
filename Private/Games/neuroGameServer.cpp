#include "Games/neuroGameServer.h"

#include "Core/utf8.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace revia::games
{

namespace
{
using nlohmann::json;

constexpr std::size_t LongestContext = 4000;
constexpr std::size_t LongestDescription = 2000;
constexpr std::size_t LongestSchema = 8000;

std::string Text(const json& value, const char* key)
{
    if (!value.is_object() || !value.contains(key)) return {};
    const json& field = value[key];
    return field.is_string() ? field.get<std::string>() : std::string();
}

bool Validate(const json& data, const json& schema, const std::string& where, std::string& outError)
{
    if (!schema.is_object()) return true;
    const std::string type = Text(schema, "type");
    const auto fail = [&](const std::string& why)
    {
        outError = (where.empty() ? std::string("value") : where) + " " + why;
        return false;
    };
    if (!type.empty())
    {
        const bool ok =
            (type == "object" && data.is_object()) || (type == "array" && data.is_array()) ||
            (type == "string" && data.is_string()) || (type == "boolean" && data.is_boolean()) ||
            (type == "null" && data.is_null()) || (type == "number" && data.is_number()) ||
            (type == "integer" && data.is_number_integer());
        if (!ok) return fail("must be of type " + type);
    }
    if (schema.contains("enum") && schema["enum"].is_array())
    {
        const auto& allowed = schema["enum"];
        if (std::none_of(allowed.begin(), allowed.end(), [&](const json& option) { return option == data; }))
        {
            return fail("must be one of " + allowed.dump());
        }
    }
    if (schema.contains("const") && schema["const"] != data) return fail("must equal " + schema["const"].dump());
    if (data.is_number())
    {
        if (schema.contains("minimum") && schema["minimum"].is_number() && data.get<double>() < schema["minimum"].get<double>())
            return fail("must be at least " + schema["minimum"].dump());
        if (schema.contains("maximum") && schema["maximum"].is_number() && data.get<double>() > schema["maximum"].get<double>())
            return fail("must be at most " + schema["maximum"].dump());
    }
    if (data.is_string())
    {
        const std::size_t length = data.get<std::string>().size();
        if (schema.contains("minLength") && schema["minLength"].is_number_integer() &&
            length < schema["minLength"].get<std::size_t>())
            return fail("is too short");
        if (schema.contains("maxLength") && schema["maxLength"].is_number_integer() &&
            length > schema["maxLength"].get<std::size_t>())
            return fail("is too long");
    }
    if (data.is_array())
    {
        if (schema.contains("minItems") && schema["minItems"].is_number_integer() &&
            data.size() < schema["minItems"].get<std::size_t>())
            return fail("has too few items");
        if (schema.contains("maxItems") && schema["maxItems"].is_number_integer() &&
            data.size() > schema["maxItems"].get<std::size_t>())
            return fail("has too many items");
        if (schema.contains("items") && schema["items"].is_object())
        {
            for (std::size_t index = 0; index < data.size(); ++index)
            {
                if (!Validate(data[index], schema["items"], where + "[" + std::to_string(index) + "]", outError))
                    return false;
            }
        }
    }
    if (data.is_object())
    {
        const json properties = schema.value("properties", json::object());
        if (schema.contains("required") && schema["required"].is_array())
        {
            for (const json& required : schema["required"])
            {
                if (required.is_string() && !data.contains(required.get<std::string>()))
                    return fail("is missing " + required.get<std::string>());
            }
        }
        for (const auto& [key, value] : data.items())
        {
            if (properties.is_object() && properties.contains(key))
            {
                if (!Validate(value, properties[key], where.empty() ? key : where + "." + key, outError)) return false;
            }
            else if (schema.contains("additionalProperties") && schema["additionalProperties"].is_boolean() &&
                !schema["additionalProperties"].get<bool>())
            {
                return fail("has an unexpected field " + key);
            }
        }
    }
    return true;
}

json Minimal(const json& schema)
{
    if (!schema.is_object()) return json::object();
    if (schema.contains("const")) return schema["const"];
    if (schema.contains("enum") && schema["enum"].is_array() && !schema["enum"].empty()) return schema["enum"].front();
    const std::string type = Text(schema, "type");
    if (type == "object" || type.empty())
    {
        json object = json::object();
        const json properties = schema.value("properties", json::object());
        if (schema.contains("required") && schema["required"].is_array())
        {
            for (const json& required : schema["required"])
            {
                if (!required.is_string()) continue;
                const std::string key = required.get<std::string>();
                object[key] = properties.is_object() && properties.contains(key) ? Minimal(properties[key]) : json();
            }
        }
        return object;
    }
    if (type == "string")
    {
        const std::size_t shortest = schema.contains("minLength") && schema["minLength"].is_number_integer()
            ? schema["minLength"].get<std::size_t>() : 0;
        return std::string(shortest, 'a');
    }
    if (type == "number" || type == "integer")
    {
        if (schema.contains("minimum") && schema["minimum"].is_number()) return schema["minimum"];
        if (schema.contains("maximum") && schema["maximum"].is_number() && schema["maximum"].get<double>() < 0)
            return schema["maximum"];
        return 0;
    }
    if (type == "boolean") return false;
    if (type == "array")
    {
        json items = json::array();
        const std::size_t fewest = schema.contains("minItems") && schema["minItems"].is_number_integer()
            ? schema["minItems"].get<std::size_t>() : 0;
        for (std::size_t index = 0; index < fewest; ++index)
        {
            items.push_back(schema.contains("items") ? Minimal(schema["items"]) : json());
        }
        return items;
    }
    return json();
}
} // namespace

NeuroGameServer::NeuroGameServer()
{
    server.SetHandlers(
        [this](const net::WebSocketServer::ConnectionId connection, const std::string& text)
        {
            OnMessage(connection, text);
        },
        [this](const net::WebSocketServer::ConnectionId connection, const bool connected)
        {
            OnConnection(connection, connected);
        });
}

NeuroGameServer::~NeuroGameServer()
{
    Stop();
}

void NeuroGameServer::SetEventHandler(EventHandler inputHandler)
{
    std::lock_guard lock(mutex);
    handler = std::move(inputHandler);
}

bool NeuroGameServer::Start(const std::string& host, const std::uint16_t port, std::string& outError)
{
    return server.Start(host, port, outError);
}

void NeuroGameServer::Stop()
{
    server.Stop();
    std::lock_guard lock(mutex);
    games.clear();
    results.clear();
}

bool NeuroGameServer::ValidActionName(const std::string& name)
{
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](const unsigned char character)
    {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
            character == '_' || character == '-';
    });
}

bool NeuroGameServer::ValidateAgainstSchema(
    const std::string& dataJson, const std::string& schemaJson, std::string& outError)
{
    json data;
    json schema;
    try
    {
        data = dataJson.empty() ? json::object() : json::parse(dataJson);
        schema = schemaJson.empty() ? json::object() : json::parse(schemaJson);
    }
    catch (const std::exception& error)
    {
        outError = std::string("not JSON: ") + error.what();
        return false;
    }
    return Validate(data, schema, "", outError);
}

std::string NeuroGameServer::MinimalDataFor(const std::string& schemaJson)
{
    try
    {
        const json schema = schemaJson.empty() ? json::object() : json::parse(schemaJson);
        return Minimal(schema).dump();
    }
    catch (const std::exception&)
    {
        return "{}";
    }
}

GameSnapshot NeuroGameServer::Snapshot(const GameState& state)
{
    GameSnapshot snapshot;
    snapshot.name = state.name;
    snapshot.connection = state.connection;
    snapshot.actions = state.actions;
    snapshot.context = state.context;
    snapshot.force = state.force;
    return snapshot;
}

std::vector<GameSnapshot> NeuroGameServer::Games() const
{
    std::lock_guard lock(mutex);
    std::vector<GameSnapshot> snapshots;
    for (const auto& [connection, state] : games) snapshots.push_back(Snapshot(state));
    return snapshots;
}

std::optional<GameSnapshot> NeuroGameServer::Game(const std::string& name) const
{
    std::lock_guard lock(mutex);
    for (const auto& [connection, state] : games)
    {
        if (state.name == name) return Snapshot(state);
    }
    return std::nullopt;
}

void NeuroGameServer::Emit(const GameEvent& event)
{
    EventHandler current;
    {
        std::lock_guard lock(mutex);
        current = handler;
    }
    if (current) current(event);
}

void NeuroGameServer::OnConnection(const net::WebSocketServer::ConnectionId connection, const bool connected)
{
    if (connected) return;
    std::string name;
    {
        std::lock_guard lock(mutex);
        const auto found = games.find(connection);
        if (found == games.end()) return;
        name = found->second.name;
        games.erase(found);
    }
    GameEvent event;
    event.kind = GameEvent::Kind::Disconnected;
    event.game = name;
    Emit(event);
}

void NeuroGameServer::OnMessage(const net::WebSocketServer::ConnectionId connection, const std::string& text)
{
    json message;
    try
    {
        message = json::parse(text);
    }
    catch (const std::exception&)
    {
        return;
    }
    if (!message.is_object()) return;
    const std::string command = Text(message, "command");
    const std::string gameName = utf8::Prefix(Text(message, "game"), 120);
    const json data = message.value("data", json::object());
    if (command.empty()) return;

    GameEvent event;
    bool emit = false;
    {
        std::lock_guard lock(mutex);
        GameState& state = games[connection];
        state.connection = connection;
        if (state.name.empty() && !gameName.empty()) state.name = gameName;
        event.game = state.name;

        if (command == "startup")
        {
            state.name = gameName.empty() ? state.name : gameName;
            state.actions.clear();
            state.context.clear();
            state.force.reset();
            event.game = state.name;
            event.kind = GameEvent::Kind::Connected;
            emit = true;
            (void)server.Send(connection, json({
                {"command", "startup"},
                {"data", {{"session", {
                    {"sessionId", "revia-" + std::to_string(connection)},
                    {"characterId", "revia"},
                    {"displayName", "Revia"}}}}}}).dump());
        }
        else if (command == "context")
        {
            ContextEntry entry;
            entry.message = utf8::Prefix(Text(data, "message"), LongestContext);
            entry.silent = data.is_object() && data.value("silent", false);
            if (!entry.message.empty())
            {
                state.context.push_back(entry);
                while (state.context.size() > MaximumContext) state.context.erase(state.context.begin());
                event.kind = GameEvent::Kind::Context;
                event.text = entry.message;
                event.silent = entry.silent;
                emit = true;
            }
        }
        else if (command == "actions/register")
        {
            if (data.is_object() && data.contains("actions") && data["actions"].is_array())
            {
                for (const json& raw : data["actions"])
                {
                    if (!raw.is_object()) continue;
                    GameAction action;
                    action.name = Text(raw, "name");
                    action.description = utf8::Prefix(Text(raw, "description"), LongestDescription);
                    if (!ValidActionName(action.name)) continue;
                    if (raw.contains("schema") && raw["schema"].is_object())
                    {
                        if (Text(raw["schema"], "type") != "object") continue;
                        action.schema = utf8::Prefix(raw["schema"].dump(), LongestSchema);
                    }
                    const auto existing = std::find_if(state.actions.begin(), state.actions.end(),
                        [&](const GameAction& candidate) { return candidate.name == action.name; });
                    if (existing != state.actions.end()) *existing = action;
                    else if (state.actions.size() < MaximumActions) state.actions.push_back(action);
                }
                event.kind = GameEvent::Kind::ActionsChanged;
                event.text = std::to_string(state.actions.size()) + " actions";
                emit = true;
            }
        }
        else if (command == "actions/unregister")
        {
            if (data.is_object() && data.contains("action_names") && data["action_names"].is_array())
            {
                for (const json& raw : data["action_names"])
                {
                    if (!raw.is_string()) continue;
                    const std::string name = raw.get<std::string>();
                    state.actions.erase(std::remove_if(state.actions.begin(), state.actions.end(),
                        [&](const GameAction& candidate) { return candidate.name == name; }), state.actions.end());
                }
                event.kind = GameEvent::Kind::ActionsChanged;
                event.text = std::to_string(state.actions.size()) + " actions";
                emit = true;
            }
        }
        else if (command == "actions/force")
        {
            ForceRequest force;
            force.serial = nextForceSerial++;
            force.query = utf8::Prefix(Text(data, "query"), LongestContext);
            force.state = utf8::Prefix(Text(data, "state"), LongestContext);
            force.priority = Text(data, "priority");
            force.ephemeralContext = data.is_object() && data.value("ephemeral_context", false);
            if (data.is_object() && data.contains("action_names") && data["action_names"].is_array())
            {
                for (const json& raw : data["action_names"])
                {
                    if (raw.is_string()) force.actionNames.push_back(raw.get<std::string>());
                }
            }
            // Only actions the game registered can be forced.
            force.actionNames.erase(std::remove_if(force.actionNames.begin(), force.actionNames.end(),
                [&](const std::string& name)
                {
                    return std::none_of(state.actions.begin(), state.actions.end(),
                        [&](const GameAction& candidate) { return candidate.name == name; });
                }), force.actionNames.end());
            if (!force.actionNames.empty())
            {
                state.force = force;
                event.kind = GameEvent::Kind::Force;
                event.text = force.query;
                event.serial = force.serial;
                emit = true;
            }
        }
        else if (command == "action/result")
        {
            ActionResultMessage result;
            result.id = Text(data, "id");
            result.success = data.is_object() && data.value("success", false);
            result.message = utf8::Prefix(Text(data, "message"), LongestContext);
            if (!result.id.empty())
            {
                results[result.id] = result;
                resultsReady.notify_all();
                event.kind = GameEvent::Kind::Result;
                event.text = result.message;
                event.silent = !result.success;
                emit = true;
            }
        }
    }
    if (emit) Emit(event);
}

std::string NeuroGameServer::SendAction(
    const std::string& game, const std::string& name, const std::string& dataJson, std::string& outError)
{
    outError.clear();
    net::WebSocketServer::ConnectionId connection = 0;
    std::string id;
    {
        std::lock_guard lock(mutex);
        const GameState* state = nullptr;
        for (const auto& [candidateConnection, candidate] : games)
        {
            if (candidate.name == game) state = &candidate;
        }
        if (state == nullptr)
        {
            outError = "The game is not connected.";
            return {};
        }
        const auto action = std::find_if(state->actions.begin(), state->actions.end(),
            [&](const GameAction& candidate) { return candidate.name == name; });
        if (action == state->actions.end())
        {
            outError = "The game has no action named " + name + ".";
            return {};
        }
        std::string why;
        if (!ValidateAgainstSchema(dataJson, action->schema, why))
        {
            outError = "The action's data does not fit its schema: " + why;
            return {};
        }
        connection = state->connection;
        id = "revia-action-" + std::to_string(nextActionId++);
    }
    const json message = {
        {"command", "action"},
        {"data", {{"id", id}, {"name", name}, {"data", dataJson.empty() ? "{}" : dataJson}}}};
    if (!server.Send(connection, message.dump()))
    {
        outError = "The action could not be sent; the game may have gone.";
        return {};
    }
    return id;
}

std::optional<ActionResultMessage> NeuroGameServer::WaitForResult(
    const std::string& actionId, const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        const auto found = results.find(actionId);
        if (found != results.end())
        {
            ActionResultMessage result = found->second;
            results.erase(found);
            return result;
        }
        if (resultsReady.wait_until(lock, deadline) == std::cv_status::timeout)
        {
            if (results.find(actionId) == results.end()) return std::nullopt;
        }
    }
}

void NeuroGameServer::ClearForce(const std::string& game, const std::uint64_t serial)
{
    std::lock_guard lock(mutex);
    for (auto& [connection, state] : games)
    {
        if (state.name == game && state.force && state.force->serial == serial) state.force.reset();
    }
}

bool NeuroGameServer::SendSpeechFinished(const std::string& game, const bool isFinal)
{
    net::WebSocketServer::ConnectionId connection = 0;
    {
        std::lock_guard lock(mutex);
        for (const auto& [candidateConnection, state] : games)
        {
            if (state.name == game) connection = candidateConnection;
        }
    }
    if (connection == 0) return false;
    return server.Send(connection,
        json({{"command", "speech_finished"}, {"data", {{"isFinal", isFinal}}}}).dump());
}

void NeuroGameServer::Disconnect(const std::string& game)
{
    net::WebSocketServer::ConnectionId connection = 0;
    {
        std::lock_guard lock(mutex);
        for (const auto& [candidateConnection, state] : games)
        {
            if (state.name == game) connection = candidateConnection;
        }
    }
    if (connection != 0) server.Close(connection);
}

} // namespace revia::games
