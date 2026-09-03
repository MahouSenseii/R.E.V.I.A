#include "Resources/loadGovernor.h"

#include <algorithm>
#include <cstddef>
#include <sstream>

namespace revia::resources
{

std::string ToString(const LoadState state)
{
    switch (state)
    {
        case LoadState::Free: return "free";
        case LoadState::Normal: return "normal";
        case LoadState::Pressured: return "pressured";
        case LoadState::Throttled: return "throttled";
    }
    return "normal";
}

namespace
{

// A meter contributes to the shedding decision only if it can run out.
//
// Percent-unit meters are engine utilisation: already a fraction of their own device,
// and a statement about how busy something is rather than how full. Counting them would
// throttle Revia for generating, which is the work, not a fault.
bool CanRunOut(const UsageMeter& meter)
{
    return meter.unit != MeterUnit::Percent && meter.capacity > 0.0;
}

bool Contributes(const UsageMeter& meter, const bool ignoreUnmeasured)
{
    return (!ignoreUnmeasured || meter.measured) && CanRunOut(meter);
}

} // namespace

double PeakCapacityPressure(const UsageSnapshot& usage, const bool ignoreUnmeasured)
{
    double peak = 0.0;
    for (const UsageMeter& meter : usage.meters)
    {
        if (!Contributes(meter, ignoreUnmeasured))
        {
            continue;
        }
        peak = std::max(peak, meter.used / meter.capacity);
    }
    return peak;
}

double PeakBudgetUtilisation(const UsageSnapshot& usage, const bool ignoreUnmeasured)
{
    double peak = 0.0;
    for (const UsageMeter& meter : usage.meters)
    {
        if (ignoreUnmeasured && !meter.measured)
        {
            continue;
        }
        if (meter.budget <= 0.0)
        {
            // A meter with no budget has nothing to be over, so it cannot contribute a
            // ratio. Treating it as fully used would report an overrun that never was.
            continue;
        }
        peak = std::max(peak, meter.used / meter.budget);
    }
    return peak;
}

LoadAdjustment AssessLoad(const UsageSnapshot& usage, const LoadThresholds& thresholds)
{
    LoadAdjustment adjustment;

    if (!usage.measured || usage.meters.empty())
    {
        // Nothing was measured. Behave exactly as normal rather than guessing: assuming
        // idle would invite the machine to take on work it cannot carry, and assuming
        // busy would make an unmeasurable platform permanently degraded.
        adjustment.state = LoadState::Normal;
        adjustment.reason =
            "No usable resource readings, so nothing is being held back or added.";
        return adjustment;
    }

    // Count what actually contributed. Without this an all-unmeasured meter set produces
    // a peak of zero and the machine is declared free -- the precise "assume idle"
    // failure this governor is supposed to avoid, and the one that invites work the
    // machine may not be able to carry.
    std::size_t contributing = 0;
    for (const UsageMeter& meter : usage.meters)
    {
        if (Contributes(meter, thresholds.ignoreUnmeasured))
        {
            ++contributing;
        }
    }
    if (contributing == 0)
    {
        adjustment.state = LoadState::Normal;
        adjustment.reason =
            "No meter could be read, so nothing is being held back or added.";
        return adjustment;
    }

    const double peak = PeakCapacityPressure(usage, thresholds.ignoreUnmeasured);
    adjustment.budgetExceeded =
        PeakBudgetUtilisation(usage, thresholds.ignoreUnmeasured) > 1.0;

    std::ostringstream reason;
    const int percent = static_cast<int>(peak * 100.0);

    if (peak > thresholds.throttledAbove)
    {
        adjustment.state = LoadState::Throttled;
        // Everything optional stops. Nothing already running is cancelled: killing a
        // reply mid-sentence to save memory is a worse outcome than finishing it.
        adjustment.voicePrefetchFragments = 1;
        adjustment.allowPhraseAheadVoice = false;
        adjustment.allowOptionalBackgroundWork = false;
        adjustment.allowOpportunisticVision = false;
        reason << "A device is " << percent
               << "% full, so optional work is on hold until something is released.";
    }
    else if (peak > thresholds.pressuredAbove)
    {
        adjustment.state = LoadState::Pressured;
        adjustment.voicePrefetchFragments = 2;
        adjustment.allowPhraseAheadVoice = false;
        // Background work stops before conversation quality does. Curiosity planning and
        // memory consolidation are the things a person will not miss; a stuttering voice
        // is the thing they will.
        adjustment.allowOptionalBackgroundWork = false;
        adjustment.allowOpportunisticVision = true;
        reason << "A device is " << percent
               << "% full, so background work is paused to protect the reply.";
    }
    else if (peak < thresholds.freeBelow)
    {
        adjustment.state = LoadState::Free;
        // Deeper prefetch is the one thing genuinely worth buying with spare capacity:
        // it shortens the gap between spoken sentences.
        adjustment.voicePrefetchFragments = 5;
        adjustment.allowPhraseAheadVoice = true;
        adjustment.allowOptionalBackgroundWork = true;
        adjustment.allowOpportunisticVision = true;
        reason << "The busiest device is only " << percent
               << "% full, so there is room to work further ahead.";
    }
    else
    {
        adjustment.state = LoadState::Normal;
        reason << "The busiest device is " << percent << "% full.";
    }

    // Said alongside the state, never instead of it. Resident model weights put Revia
    // over an allowance that was carved out before they loaded, and that is worth
    // knowing; it is not a reason to stop doing things, and reading it as one is what
    // switched her background work off for entire sessions at a time.
    if (adjustment.budgetExceeded)
    {
        reason << " Revia is over her configured budget, which is a planning result "
                  "rather than a hardware fault.";
    }

    adjustment.reason = reason.str();
    return adjustment;
}

} // namespace revia::resources
