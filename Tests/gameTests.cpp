#include "reviaSessionTestAccess.h"
#include "Core/stdioProcess.h"
#include "Games/gameActionExecutor.h"
#include "Games/gamePlayer.h"
#include "Games/neuroGameServer.h"
#include "Net/webSocket.h"
#include "Policy/capabilityPolicy.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>

namespace
{
using revia::games::ForceRequest;
using revia::games::GameDecision;
using revia::games::GameEvent;
using revia::games::GameSnapshot;
using revia::games::NeuroGameServer;
using revia::runtime::ReviaSession;
using revia::runtime::RuntimeEvent;
using revia::runtime::RuntimeEventKind;
using revia::tests::Check;
using Access = revia::runtime::ReviaSessionTestAccess;
using namespace std::chrono_literals;

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

std::string Hex(const std::string& bytes)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const unsigned char byte : bytes)
    {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 15]);
    }
    return out;
}

const char* Python()
{
#ifdef _WIN32
    return "python";
#else
    return "python3";
#endif
}

std::filesystem::path FakeGameScript()
{
    return std::filesystem::path(__FILE__).parent_path() / "fakeNeuroGame.py";
}

// Reads the fake game's stdout until `wanted` appears, or the time is up.
bool WaitForLine(revia::core::StdioProcess& game, const std::string& wanted,
    std::vector<std::string>& lines, const std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::string line;
        bool closed = false;
        if (game.ReadLine(line, 200ms, closed))
        {
            lines.push_back(line);
            if (Contains(line, wanted)) return true;
        }
        else if (closed)
        {
            return false;
        }
    }
    return false;
}

void TestTheHandshakeMatchesTheStandard()
{
    Check(Hex(revia::net::Sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 of abc is wrong.");
    Check(Hex(revia::net::Sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1 of nothing is wrong.");
    Check(Hex(revia::net::Sha1(std::string(1000, 'a'))) == "291e9a6c66994949b57ba5e650361e98fc36b1ba",
        "SHA-1 of a longer input is wrong.");
    Check(revia::net::Base64Encode("") == "" && revia::net::Base64Encode("f") == "Zg==" &&
            revia::net::Base64Encode("fo") == "Zm8=" && revia::net::Base64Encode("foo") == "Zm9v" &&
            revia::net::Base64Encode("foobar") == "Zm9vYmFy",
        "Base64 does not match the RFC vectors.");
    Check(revia::net::WebSocketAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
        "The accept key does not match RFC 6455's example.");
    std::string key;
    std::string path;
    std::string error;
    Check(revia::net::ParseUpgradeRequest(
              "GET /chat HTTP/1.1\r\nHost: server.example.com\r\nUpgrade: websocket\r\n"
              "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
              "Sec-WebSocket-Version: 13\r\n\r\n", key, path, error) &&
            key == "dGhlIHNhbXBsZSBub25jZQ==" && path == "/chat",
        "A standard upgrade request was not read: " + error);
    Check(!revia::net::ParseUpgradeRequest("GET / HTTP/1.1\r\nHost: x\r\n\r\n", key, path, error) &&
            Contains(error, "not a WebSocket"),
        "A plain GET was taken for an upgrade.");
    Check(Contains(revia::net::UpgradeResponse("dGhlIHNhbXBsZSBub25jZQ=="),
              "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
        "The upgrade response lacks the accept key.");
}

void TestFramesRoundTrip()
{
    for (const std::size_t size : {std::size_t(5), std::size_t(200), std::size_t(70000)})
    {
        const std::string payload(size, static_cast<char>('a' + size % 26));
        for (const bool mask : {true, false})
        {
            std::string buffer = revia::net::EncodeFrame(revia::net::Opcode::Text, payload, mask);
            revia::net::Frame frame;
            std::string error;
            Check(revia::net::DecodeFrame(buffer, frame, error) == revia::net::DecodeResult::Frame &&
                    frame.fin && frame.opcode == revia::net::Opcode::Text && frame.payload == payload &&
                    buffer.empty(),
                "A frame of " + std::to_string(size) + " bytes did not round-trip: " + error);
        }
    }
    std::string partial = revia::net::EncodeFrame(revia::net::Opcode::Text, "hello", true);
    std::string truncated = partial.substr(0, partial.size() - 2);
    revia::net::Frame frame;
    std::string error;
    Check(revia::net::DecodeFrame(truncated, frame, error) == revia::net::DecodeResult::Incomplete,
        "A truncated frame was decoded.");
    std::string reserved = partial;
    reserved[0] = static_cast<char>(reserved[0] | 0x40);
    Check(revia::net::DecodeFrame(reserved, frame, error) == revia::net::DecodeResult::Error,
        "A reserved bit was ignored.");
    std::string huge = revia::net::EncodeFrame(revia::net::Opcode::Text, std::string(2000, 'x'), true);
    Check(revia::net::DecodeFrame(huge, frame, error, 1000) == revia::net::DecodeResult::Error,
        "An oversize frame was accepted.");
    std::string two = revia::net::EncodeFrame(revia::net::Opcode::Text, "ab", true, false) +
        revia::net::EncodeFrame(revia::net::Opcode::Continuation, "cd", true, true);
    Check(revia::net::DecodeFrame(two, frame, error) == revia::net::DecodeResult::Frame && !frame.fin &&
            frame.payload == "ab" && revia::net::DecodeFrame(two, frame, error) == revia::net::DecodeResult::Frame &&
            frame.fin && frame.opcode == revia::net::Opcode::Continuation && frame.payload == "cd",
        "Fragments were not decoded in order.");
}

void TestTheProtocolsRules()
{
    Check(NeuroGameServer::ValidActionName("join_friend-lobby2") && !NeuroGameServer::ValidActionName("Bad Name!") &&
            !NeuroGameServer::ValidActionName("") && !NeuroGameServer::ValidActionName("UPPER"),
        "Action names are not checked as the protocol says.");
    const std::string schema = R"({"type":"object","properties":{"card":{"type":"string","enum":["queen","king"]},
        "bet":{"type":"integer","minimum":1,"maximum":10}},"required":["card"],"additionalProperties":false})";
    std::string why;
    Check(NeuroGameServer::ValidateAgainstSchema(R"({"card":"queen","bet":3})", schema, why), "Valid data was refused: " + why);
    Check(!NeuroGameServer::ValidateAgainstSchema(R"({"card":"ace"})", schema, why) && Contains(why, "one of"),
        "A value outside the enum was accepted.");
    Check(!NeuroGameServer::ValidateAgainstSchema(R"({"bet":3})", schema, why) && Contains(why, "missing card"),
        "A missing required field was accepted.");
    Check(!NeuroGameServer::ValidateAgainstSchema(R"({"card":"queen","bet":11})", schema, why) && Contains(why, "at most"),
        "A number over its maximum was accepted.");
    Check(!NeuroGameServer::ValidateAgainstSchema(R"({"card":"queen","extra":1})", schema, why) && Contains(why, "unexpected"),
        "An extra field was accepted with additionalProperties false.");
    Check(!NeuroGameServer::ValidateAgainstSchema("not json", schema, why), "Non-JSON data was accepted.");
    Check(NeuroGameServer::ValidateAgainstSchema("{}", "{}", why), "Empty data against no schema was refused.");
    Check(NeuroGameServer::MinimalDataFor(schema) == R"({"card":"queen"})",
        "The least data for the schema is wrong: " + NeuroGameServer::MinimalDataFor(schema));
    Check(NeuroGameServer::MinimalDataFor(R"({"type":"object","properties":{"n":{"type":"integer","minimum":2},
        "s":{"type":"string","minLength":2},"b":{"type":"boolean"}},"required":["n","s","b"]})") ==
            R"({"b":false,"n":2,"s":"aa"})",
        "The least data did not honour minimums.");
    Check(NeuroGameServer::MinimalDataFor("{}") == "{}", "The least data for no schema is not an empty object.");

    const std::string decisionSchema = revia::games::DecisionSchema({"play_card", "fold"}, false);
    Check(Contains(decisionSchema, R"("enum":["play_card","fold"])") && !Contains(decisionSchema, "\"act\""),
        "The forced decision schema is wrong: " + decisionSchema);
    Check(Contains(revia::games::DecisionSchema({"fold"}, true), "\"act\""), "The free-turn schema lacks act.");
    GameDecision decision;
    std::string error;
    Check(revia::games::ParseDecision(R"({"name":"play_card","data":{"card":"king"},"say":"Take that."})", decision, error) &&
            decision.act && decision.name == "play_card" && decision.dataJson == R"({"card":"king"})" &&
            decision.say == "Take that." && decision.source == "model",
        "A decision was not parsed: " + error);
    Check(revia::games::ParseDecision(R"({"act":false,"name":"","data":{},"say":""})", decision, error) && !decision.act,
        "A pass was not parsed.");
    Check(!revia::games::ParseDecision(R"({"data":{}})", decision, error) && Contains(error, "named no action"),
        "A decision with no action was accepted.");
    Check(!revia::games::ParseDecision("I'd play the king.", decision, error), "Prose was parsed as a decision.");

    GameSnapshot game;
    game.name = "Fake";
    game.actions = {{"play_card", "Play.", schema}, {"fold", "Fold.", "{}"}};
    const GameDecision fallback = revia::games::FallbackDecision(game, {"fold", "play_card"});
    Check(fallback.act && fallback.name == "fold" && fallback.dataJson == "{}" && fallback.source == "fallback",
        "The fallback did not take the first allowed action with the least data.");
    Check(revia::games::FallbackDecision(game, {}).name == "play_card" &&
            revia::games::FallbackDecision(game, {}).dataJson == R"({"card":"queen"})",
        "The fallback with no restriction is wrong.");
    Check(!revia::games::FallbackDecision(game, {"unknown"}).act, "A fallback acted with no allowed action.");
    ForceRequest force;
    force.query = "Play or fold.";
    force.state = "Round 1";
    force.actionNames = {"fold"};
    game.context.push_back({"You hold a queen.", false});
    const std::string envelope = revia::games::BuildForceEnvelope(game, force, "The card must be a queen");
    Check(Contains(envelope, "Game: Fake") && Contains(envelope, "You hold a queen.") && Contains(envelope, "Round 1") &&
            Contains(envelope, "- fold: Fold.; no data") && !Contains(envelope, "- play_card") &&
            Contains(envelope, "rejected by the game: The card must be a queen"),
        "The force envelope is wrong:\n" + envelope);
    Check(Contains(revia::games::BuildTurnEnvelope(game), "not waiting on you") &&
            Contains(revia::games::BuildTurnEnvelope(game), "- play_card: Play.; data {"),
        "The free-turn envelope is wrong.");
}

struct EventLog
{
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<GameEvent> events;

    bool WaitFor(const GameEvent::Kind kind, GameEvent& out, const std::chrono::seconds timeout)
    {
        std::unique_lock lock(mutex);
        std::size_t seen = 0;
        return ready.wait_for(lock, timeout, [&]
        {
            for (; seen < events.size(); ++seen)
            {
                if (events[seen].kind == kind)
                {
                    out = events[seen];
                    events.erase(events.begin() + static_cast<std::ptrdiff_t>(seen));
                    return true;
                }
            }
            return false;
        });
    }
};

void TestAGameConnectsRegistersAndIsAnswered()
{
    NeuroGameServer server;
    EventLog log;
    server.SetEventHandler([&](const GameEvent& event)
    {
        {
            std::lock_guard lock(log.mutex);
            log.events.push_back(event);
        }
        log.ready.notify_all();
    });
    std::string error;
    Check(server.Start("127.0.0.1", 0, error) && server.Port() != 0, "The game server did not start: " + error);

    revia::core::StdioProcess game;
    revia::core::StdioLaunch launch;
    launch.command = Python();
    launch.arguments = {FakeGameScript().string(), std::to_string(server.Port())};
    launch.logName = "fake-neuro-game";
    launch.displayName = "fake game";
    if (!game.Start(launch, error))
    {
#ifdef _WIN32
        std::cout << "Game protocol test skipped: " << error << "\n";
        return;
#else
        Check(false, "The fake game did not start: " + error);
#endif
    }
    std::vector<std::string> lines;
    Check(WaitForLine(game, "HANDSHAKE OK", lines, 10s), "The fake game's handshake failed: " +
        (lines.empty() ? std::string("no output") : lines.back()));

    GameEvent event;
    Check(log.WaitFor(GameEvent::Kind::Connected, event, 10s) && event.game == "Fake Card Game",
        "The game's startup was not seen.");
    Check(log.WaitFor(GameEvent::Kind::Force, event, 10s) && event.text == "Play a card or fold.",
        "The forced choice was not seen.");
    std::optional<GameSnapshot> snapshot = server.Game("Fake Card Game");
    Check(snapshot && snapshot->actions.size() == 2 && snapshot->force &&
            snapshot->force->actionNames == std::vector<std::string>{"play_card", "fold"} &&
            snapshot->force->state == "Round 1" && snapshot->force->priority == "high" &&
            snapshot->context.size() == 1 && Contains(snapshot->context.front().message, "your turn"),
        "The game's actions, context and force were not kept as sent (a bad name and an "
        "unregistered action must be dropped).");

    // The move: what she would send with no brain, then the game's verdict.
    const GameDecision move = revia::games::FallbackDecision(*snapshot, snapshot->force->actionNames);
    Check(server.SendAction("Fake Card Game", "nope", "{}", error).empty() && Contains(error, "no action named"),
        "An unregistered action was sent.");
    Check(server.SendAction("Fake Card Game", "play_card", R"({"card":"ace"})", error).empty() &&
            Contains(error, "does not fit"),
        "Data outside the schema was sent.");
    const std::string id = server.SendAction("Fake Card Game", move.name, move.dataJson, error);
    Check(!id.empty(), "The move was not sent: " + error);
    const auto verdict = server.WaitForResult(id, 10s);
    Check(verdict && verdict->success && verdict->message == "played", "The game's verdict did not arrive.");
    server.ClearForce("Fake Card Game", snapshot->force->serial);
    Check(!server.Game("Fake Card Game")->force, "The answered force stayed pending.");

    Check(log.WaitFor(GameEvent::Kind::Force, event, 10s) && event.text == "Fold.", "The second force was not seen.");
    snapshot = server.Game("Fake Card Game");
    Check(snapshot && snapshot->force && snapshot->force->actionNames == std::vector<std::string>{"fold"},
        "The second force did not narrow the actions.");
    const std::string second = server.SendAction("Fake Card Game", "fold", "{}", error);
    const auto secondVerdict = server.WaitForResult(second, 10s);
    Check(secondVerdict && secondVerdict->success, "The fold was not accepted.");
    Check(server.SendSpeechFinished("Fake Card Game"), "speech_finished could not be sent.");

    Check(WaitForLine(game, "DONE", lines, 15s), "The fake game did not finish.");
    bool played = false;
    bool folded = false;
    bool speech = false;
    for (const std::string& line : lines)
    {
        if (line == R"(ACTION play_card {"card": "queen"})") played = true;
        if (line == "ACTION fold {}") folded = true;
        if (line == "OTHER speech_finished") speech = true;
    }
    Check(played && folded && speech, "The fake game did not receive what was sent.");
    Check(log.WaitFor(GameEvent::Kind::Disconnected, event, 10s), "The game's leaving was not seen.");
    Check(server.Games().empty(), "A gone game stayed listed.");
    game.Stop();
    server.Stop();
}

void TestGameActionsPassPolicyAndTheExecutor()
{
    using revia::actions::ActionRequest;
    using revia::actions::ActionType;
    using revia::actions::CapabilitySettings;
    using revia::actions::PolicyVerdict;
    using revia::actions::RiskLevel;
    CapabilitySettings settings;
    ActionRequest move;
    move.type = ActionType::GameAction;
    move.application = "Fake Card Game";
    move.value = "fold";
    {
        revia::policy::CapabilityPolicy policy(settings);
        const auto decision = policy.Evaluate(move);
        Check(decision.verdict == PolicyVerdict::Blocked && Contains(decision.reason, "disabled"),
            "A game action was admitted with games off.");
    }
    settings.games.enabled = true;
    settings.games.approvedGames = {"Other Game"};
    {
        revia::policy::CapabilityPolicy policy(settings);
        Check(policy.Evaluate(move).verdict == PolicyVerdict::Blocked, "An unapproved game was admitted.");
    }
    settings.games.approvedGames = {"fake card game"};
    {
        revia::policy::CapabilityPolicy policy(settings);
        const auto decision = policy.Evaluate(move);
        Check(decision.verdict == PolicyVerdict::Allowed && decision.risk == RiskLevel::ReadOnly,
            "An approved game action was not admitted read-only: " + decision.reason);
        ActionRequest nameless = move;
        nameless.value.clear();
        Check(policy.Evaluate(nameless).verdict == PolicyVerdict::Blocked, "A nameless game action was admitted.");
    }
    settings.games.approvedGames.clear();
    {
        revia::policy::CapabilityPolicy policy(settings);
        Check(policy.Evaluate(move).verdict == PolicyVerdict::Allowed, "An empty list did not admit any game.");
    }

    // Anti-cheat titles are refused by the desktop path whatever else is approved.
    CapabilitySettings desktop;
    desktop.approvedApplications = {"VALORANT-Win64-Shipping.exe", "notepad.exe"};
    desktop.approvedControls = {{"VALORANT-Win64-Shipping.exe", {"*"}}, {"notepad.exe", {"*"}}};
    desktop.desktopControl.keyboard = true;
    desktop.desktopControl.pointer = true;
    revia::policy::CapabilityPolicy policy(desktop);
    ActionRequest keys;
    keys.type = ActionType::PressKeys;
    keys.application = "VALORANT-Win64-Shipping.exe";
    keys.value = "space";
    auto decision = policy.Evaluate(keys);
    Check(decision.verdict == PolicyVerdict::Blocked && Contains(decision.reason, "anti-cheat"),
        "Keys to an anti-cheat title were not refused: " + decision.reason);
    keys.application = "C:\\Riot Games\\VALORANT\\live\\ShooterGame\\Binaries\\Win64\\VALORANT-Win64-Shipping.exe";
    Check(Contains(policy.Evaluate(keys).reason, "anti-cheat"), "A full path to an anti-cheat title slipped past.");
    keys.application = "notepad.exe";
    Check(!Contains(policy.Evaluate(keys).reason, "anti-cheat"), "An ordinary application was called anti-cheat.");
    ActionRequest launch;
    launch.type = ActionType::LaunchApplication;
    launch.application = "r5apex.exe";
    Check(Contains(policy.Evaluate(launch).reason, "anti-cheat"), "Launching an anti-cheat title was not refused.");

    auto server = std::make_shared<NeuroGameServer>();
    revia::games::GameActionExecutor executor(server);
    Check(executor.Handles(ActionType::GameAction) && !executor.Handles(ActionType::McpTool),
        "The executor handles the wrong actions.");
    const auto result = executor.Execute(move, revia::actions::PolicyDecision{});
    Check(result.attempted && !result.succeeded && Contains(result.message, "No game server"),
        "The executor did not report the missing server: " + result.message);
}

void TestSheAnswersAGameThroughTheSession()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    {
        const nlohmann::json capabilities = {
            {"mode", "supervised"},
            {"approvedRoots", {revia::actions::PathToUtf8(directory.root)}},
            {"autoApproveRiskThrough", "read_only"},
            {"createMissingApprovedRoots", false},
            {"games", {{"enabled", true}, {"approvedGames", {"Fake Card Game"}}}}};
        std::ofstream file(directory.root / "capabilities.json");
        file << capabilities.dump();
    }
    Access::PrepareActions(session, directory.root);
    std::mutex mutex;
    std::vector<std::string> phases;
    const auto id = session.Events().Subscribe([&](const RuntimeEvent& event)
    {
        if (event.kind != RuntimeEventKind::ComponentStatus || event.component != "Games") return;
        std::lock_guard lock(mutex);
        phases.push_back(event.phase + ": " + event.message);
    });
    Access::Settings(session).games.port = 0;
    Access::StartGames(session);
    const std::shared_ptr<NeuroGameServer> server = Access::GameServer(session);
    Check(server && server->IsRunning(), "The session did not start the game server.");

    revia::core::StdioProcess game;
    revia::core::StdioLaunch launch;
    launch.command = Python();
    launch.arguments = {FakeGameScript().string(), std::to_string(server->Port())};
    launch.logName = "fake-neuro-game-session";
    std::string error;
    if (!game.Start(launch, error))
    {
#ifdef _WIN32
        std::cout << "Game session test skipped: " << error << "\n";
        Access::StopGames(session);
        session.Events().Unsubscribe(id);
        return;
#else
        Check(false, "The fake game did not start: " + error);
#endif
    }
    std::vector<std::string> lines;
    Check(WaitForLine(game, "ACTION play_card", lines, 20s), "She did not answer the first force.");
    const std::string listed = Access::SubmitOperator(session, "/game").text;
    Check(Contains(listed, "Fake Card Game") && Contains(listed, "play_card") && Contains(listed, "fold"),
        "/game did not list the connected game: " + listed);
    Check(WaitForLine(game, "DONE", lines, 20s), "She did not answer the second force.");
    bool played = false;
    bool folded = false;
    for (const std::string& line : lines)
    {
        if (line == R"(ACTION play_card {"card": "queen"})") played = true;
        if (line == "ACTION fold {}") folded = true;
    }
    Check(played && folded, "The moves did not reach the game through the session.");
    {
        std::lock_guard lock(mutex);
        const auto has = [&](const std::string& part)
        {
            return std::any_of(phases.begin(), phases.end(), [&](const std::string& phase) { return Contains(phase, part); });
        };
        Check(has("Listening") && has("Connected: Fake Card Game") && has("Forced move: Fake Card Game: play_card") &&
                has("Accepted: Fake Card Game: play_card") && has("Forced move: Fake Card Game: fold"),
            "The activity feed did not show the game being played.");
    }
    game.Stop();
    Access::StopGames(session);
    Check(Contains(Access::SubmitOperator(session, "/game").text, "not running"),
        "/game after stopping did not say the server is not running.");
    session.Events().Unsubscribe(id);
}

} // namespace

void RunGameTests()
{
    TestTheHandshakeMatchesTheStandard();
    TestFramesRoundTrip();
    TestTheProtocolsRules();
    TestAGameConnectsRegistersAndIsAnswered();
    TestGameActionsPassPolicyAndTheExecutor();
    TestSheAnswersAGameThroughTheSession();
    std::cout << "A game that speaks the Neuro SDK protocol connects to her, registers its moves, "
        "and gets an answer within its own rules; anti-cheat titles are never driven.\n";
}
