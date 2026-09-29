#pragma once

#include "Actions/actionTypes.h"

#include <cstdint>
#include <string>

namespace revia::stage
{

// Her hands in a machine that is not this one.
//
// The Revia-Stage design keeps her brain, memory, policy, approvals and presence on
// the host and puts an action runtime -- the desktop executors, screenshots, the
// browser, game adapters -- in a guest she drives over a private channel. This is the
// channel's language: JSON, one message per line, in either direction. Everything the
// host sends is a typed ActionRequest that already passed her policy and, where the
// policy said so, the person's confirmation; the guest performs it and reports. The
// guest never gets a shell, a file, a credential or a say in the policy.
//
// Four tiers bound what a request may do in the guest, and both ends enforce them:
// the host refuses to send a request above the tier the owner granted, and the guest
// refuses to perform one above the tier it was started with. The guest's tier is the
// lower bound of trust, because the host is not obliged to believe the guest.
enum class StageTier
{
    // T0: look. Window inspection and observation; nothing moves.
    Observe = 0,
    // T1: act inside an approved application: controls, and pointer or keyboard
    // input that names the application it is for.
    Confined = 1,
    // T2: act on the guest's whole desktop: screen-space pointer and keyboard.
    Desktop = 2,
    // T3: change the guest: start processes. Never automatic.
    System = 3
};

[[nodiscard]] const char* ToString(StageTier tier);
[[nodiscard]] bool TierFromInt(int value, StageTier& outTier);
// The least tier a request needs, from the request alone.
[[nodiscard]] StageTier TierFor(const actions::ActionRequest& request);

struct GuestInfo
{
    std::string name;
    std::string version;
    StageTier grantedTier = StageTier::Observe;
    // The checkpoint the guest was restored from, when it knows.
    std::string checkpoint;
};

// Message lines. `Decode` reads the type and id of any of them; the typed readers
// fill the rest and refuse what is out of shape.
[[nodiscard]] std::string EncodeHello(const GuestInfo& guest);
[[nodiscard]] std::string EncodeRequest(const std::string& id, const actions::ActionRequest& request, StageTier tier);
[[nodiscard]] std::string EncodeResult(const std::string& id, const actions::ActionResult& result);
[[nodiscard]] std::string EncodeRefusal(const std::string& id, const std::string& reason);
[[nodiscard]] std::string EncodeHalt();
[[nodiscard]] std::string EncodePing();
[[nodiscard]] std::string EncodePong();

struct Envelope
{
    std::string type;
    std::string id;
};

[[nodiscard]] bool Decode(const std::string& line, Envelope& outEnvelope, std::string& outError);
[[nodiscard]] bool ReadHello(const std::string& line, GuestInfo& outGuest, std::string& outError);
[[nodiscard]] bool ReadRequest(const std::string& line, actions::ActionRequest& outRequest, StageTier& outTier, std::string& outError);
[[nodiscard]] bool ReadResult(const std::string& line, actions::ActionResult& outResult, std::string& outError);
[[nodiscard]] bool ReadRefusal(const std::string& line, std::string& outReason);

// What a result may carry back: a guest can print without end, the host cannot hold it.
constexpr std::size_t LongestResultContent = 64 * 1024;
constexpr std::size_t MostResultEntries = 200;

} // namespace revia::stage
