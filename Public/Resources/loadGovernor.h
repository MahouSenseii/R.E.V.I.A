#pragma once

#include "Resources/resourceMonitor.h"

#include <string>

namespace revia::resources
{

// Physical capacity pressure. GPU activity is considered separately for admitting work.
enum class LoadState
{
    // Plenty spare. Optional work that costs latency elsewhere is worth doing.
    Free,
    Normal,
    // Getting tight. Idle resident GPU services may still have room to work.
    Pressured,
    // Something is being starved. Shed optional work immediately.
    Throttled
};

[[nodiscard]] std::string ToString(LoadState state);

// Reversible per-request load choices; never move models, devices or budgets.
// Startup placement is not recomputed from usage readings.
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
    // Load admission applies only to opportunistic vision; explicitly requested vision is never gated here.
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

// Load thresholds use physical capacity, independently of planned budgets.
// Background admission also considers measured GPU activity and absolute headroom.
struct LoadThresholds
{
    // Below this on every meter, there is room to spare.
    double freeBelow = 0.55;
    // High: a load arriving next may not fit.
    double pressuredAbove = 0.90;
    // Critical: an allocation is about to be refused.
    double throttledAbove = 0.95;
    // A high-occupancy GPU may still run background work through resident services
    // when it has this much working room and its measured engine activity is low.
    double backgroundGpuHeadroomMiB = 512.0;
    double backgroundGpuBusyAbove = 0.55;
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
[[nodiscard]] LoadAdjustment AssessLoad(const UsageSnapshot& usage, const LoadThresholds& thresholds = {});

// Worst measured hardware occupancy fraction, 0..1, for caller-controlled hysteresis.
// Excludes GPU-engine activity percentages; busy compute is not exhausted capacity.
[[nodiscard]] double PeakCapacityPressure(const UsageSnapshot& usage, bool ignoreUnmeasured = true);

// The worst occupancy as a fraction of the allowance the plan set, 0..1+. Separate from
// pressure on purpose: it says whether the plan was optimistic, which is worth reporting
// and is never by itself a reason to stop doing things.
[[nodiscard]] double PeakBudgetUtilisation(const UsageSnapshot& usage, bool ignoreUnmeasured = true);

} // namespace revia::resources
