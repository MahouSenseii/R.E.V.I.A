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

    // Whether Revia is past the allowance her plan carved out, which is a planning
    // result rather than a hardware fault and never on its own a reason to shed work.
    // Reported so the panel and the log can say it without it being confused for
    // starvation.
    bool budgetExceeded = false;

    // Plain sentence for the log and the resources panel, so a machine that has quietly
    // reduced what it attempts can say why.
    std::string reason;
};

// Thresholds, as fractions of the physical ceiling the hardware actually has.
//
// Against capacity, never against the budget. The budget is a promise Revia made to
// herself about how much of a card to leave free; passing it means the plan was
// optimistic, not that the machine is failing. Judging shedding on the budget is what
// left a healthy card at 88% occupancy reported as "starved at 110%", with every
// optional thing switched off for the whole session.
//
// The steps match the ones the Resources panel already draws, so the word the user reads
// and the decision Revia makes come from the same number.
struct LoadThresholds
{
    // Below this on every meter, there is room to spare.
    double freeBelow = 0.55;
    // High: a load arriving next may not fit.
    double pressuredAbove = 0.90;
    // Critical: an allocation is about to be refused.
    double throttledAbove = 0.95;
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

// The worst occupancy across measured meters as a fraction of the hardware ceiling,
// 0..1. This is the number that answers "is anything about to be refused", and the one
// the shedding decision is made on. Exposed so a caller can apply its own hysteresis
// without re-deriving it.
//
// Meters that are already a percentage of their own device -- GPU engine utilisation --
// are deliberately left out. A card at 100% compute is doing exactly what it was asked
// to do; being busy is not the same as being out of room, and throttling on it would
// shed work precisely when work is happening. Space is what runs out.
[[nodiscard]] double PeakCapacityPressure(
    const UsageSnapshot& usage,
    bool ignoreUnmeasured = true);

// The worst occupancy as a fraction of the allowance the plan set, 0..1+. Separate from
// pressure on purpose: it says whether the plan was optimistic, which is worth reporting
// and is never by itself a reason to stop doing things.
[[nodiscard]] double PeakBudgetUtilisation(
    const UsageSnapshot& usage,
    bool ignoreUnmeasured = true);

} // namespace revia::resources
