#pragma once

#include "Resources/resourceMonitor.h"

#include <string>

namespace revia::resources
{

// How hard the machine is working right now.
enum class LoadState
{
    // Plenty spare. Optional work that costs latency elsewhere is worth doing.
    Free,
    Normal,
    // Getting tight. Stop starting optional work, but finish what is running.
    Pressured,
    // Something is being starved. Shed optional work immediately.
    Throttled
};

[[nodiscard]] std::string ToString(LoadState state);

// Bounded, reversible adjustments the runtime may make in response to load.
//
// Deliberately narrow. The resource planner decides model placement once at startup and
// is explicitly never re-run from a reading, because moving a worker because a number
// moved turns a reproducible plan into a feedback loop. Nothing here moves a model,
// changes a device, or alters a budget: these are per-request choices that can be made
// differently on the next request and leave no lasting state behind.
struct LoadAdjustment
{
    LoadState state = LoadState::Normal;

    // How far the voice pool may synthesise ahead of playback. Prefetch buys smoothness
    // with memory and compute, which is a good trade when free and a bad one when not.
    int voicePrefetchFragments = 3;
    // Whether a long reply may spread across a second voice worker. The worker itself
    // stays resident either way; this only decides whether a given reply uses it.
    bool allowPhraseAheadVoice = true;
    // Whether optional background work -- autonomous activity, curiosity planning,
    // memory consolidation -- may start. Work already running is never killed by this.
    bool allowOptionalBackgroundWork = true;
    // Whether a turn may spend an extra round trip on screen vision it was NOT asked
    // for -- ambient orientation, a look taken on her own initiative.
    //
    // A look the user explicitly asked for is not covered by this and is never gated on
    // load. That is part of answering them, and the shedding order here puts
    // conversation last on purpose: refusing to look because the machine is busy, while
    // still generating a reply that says she cannot see, is the worst of both.
    bool allowOpportunisticVision = true;

    // Plain sentence for the log and the resources panel, so a machine that has quietly
    // reduced what it attempts can say why.
    std::string reason;
};

// Thresholds, as fractions of the budget the plan allocated.
struct LoadThresholds
{
    // Below this on every meter, there is room to spare.
    double freeBelow = 0.55;
    double pressuredAbove = 0.85;
    double throttledAbove = 0.97;
    // A meter that cannot be measured is ignored rather than assumed idle: guessing a
    // reading is how a governor confidently makes exactly the wrong call.
    bool ignoreUnmeasured = true;
};

// Reads live usage and recommends what to attempt.
//
// Pure and stateless: same snapshot in, same recommendation out. Hysteresis lives in the
// caller, which is the only thing that knows what it was already doing -- a governor
// that remembered its own last answer would make identical inputs produce different
// advice and become impossible to reason about.
[[nodiscard]] LoadAdjustment AssessLoad(
    const UsageSnapshot& usage,
    const LoadThresholds& thresholds = {});

// The worst utilisation across measured meters, 0..1+ of budget. Exposed so a caller can
// apply its own hysteresis without re-deriving it.
[[nodiscard]] double PeakUtilisation(
    const UsageSnapshot& usage,
    bool ignoreUnmeasured = true);

} // namespace revia::resources
