#pragma once

#include "Games/neuroGameServer.h"

#include <string>
#include <vector>

namespace revia::games
{

// How she picks a move.
//
// The game says what it accepts and what is going on; she picks one action and its
// data, and may say one line while she does. The words are the model's, under a
// schema that only admits a registered action; the data is checked against the
// action's own schema before it is sent. When no brain can choose -- the model is
// down, or it answered out of shape -- a forced action still gets an answer, the
// least the schema accepts, because a game that forced a choice is waiting on one.
struct GameDecision
{
    // False when she chooses to do nothing this turn (never for a force).
    bool act = false;
    std::string name;
    std::string dataJson = "{}";
    // One short line to say out loud, or nothing.
    std::string say;
    // Where the decision came from: "model" or "fallback".
    std::string source;
};

[[nodiscard]] const char* GamePlayerPrompt();
// The schema the model answers under: name is one of the allowed actions, data an
// object, say a short string; with `mayPass`, act may be false.
[[nodiscard]] std::string DecisionSchema(const std::vector<std::string>& actionNames, bool mayPass);
// The game's state and its choices, for a force. `lastFailure` is the game's message
// from a rejected attempt, so the retry can differ.
[[nodiscard]] std::string BuildForceEnvelope(
    const GameSnapshot& game, const ForceRequest& force, const std::string& lastFailure);
// The same for a turn of her own, when the game is not forcing.
[[nodiscard]] std::string BuildTurnEnvelope(const GameSnapshot& game);
[[nodiscard]] bool ParseDecision(const std::string& response, GameDecision& out, std::string& outError);
// The least she can do: the first allowed action with the least data its schema accepts.
[[nodiscard]] GameDecision FallbackDecision(const GameSnapshot& game, const std::vector<std::string>& actionNames);
// The registered actions among `names`, in order; all of them when `names` is empty.
[[nodiscard]] std::vector<GameAction> AllowedActions(const GameSnapshot& game, const std::vector<std::string>& names);

} // namespace revia::games
