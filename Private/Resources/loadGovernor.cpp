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

bool IsGpuCompute(const UsageMeter& meter)
{
    return meter.id.starts_with("gpu:") && meter.id.ends_with(":compute") &&
        meter.unit == MeterUnit::Percent;
}

bool GpuIsBusy(const UsageSnapshot& usage, const LoadThresholds& thresholds)
{
    return std::any_of(usage.meters.begin(), usage.meters.end(), [&](const UsageMeter& meter)
    {
        return IsGpuCompute(meter) && meter.measured &&
            meter.used > thresholds.backgroundGpuBusyAbove * 100.0;
    });
}

bool HasIdleGpuHeadroom(const UsageSnapshot& usage, const LoadThresholds& thresholds)
{
    // Only GPU residency gets this exception. CPU/RAM pressure still blocks work,
    // and an unreadable engine is not evidence that a nearly full card is idle.
    bool found = false;
    for (const UsageMeter& meter : usage.meters)
    {
        if (!Contributes(meter, thresholds.ignoreUnmeasured) ||
            meter.used / meter.capacity <= thresholds.pressuredAbove)
            continue;
        if (!meter.id.starts_with("gpu:") || !meter.id.ends_with(":vram") ||
            meter.unit != MeterUnit::Mebibytes ||
            meter.capacity - meter.used < thresholds.backgroundGpuHeadroomMiB)
            return false;

        const std::string computeId = meter.id.substr(0, meter.id.size() - 5) + ":compute";
        const auto compute = std::find_if(usage.meters.begin(), usage.meters.end(),
            [&](const UsageMeter& candidate) { return candidate.id == computeId; });
        if (compute == usage.meters.end() || !compute->measured ||
            !IsGpuCompute(*compute) ||
            compute->used > thresholds.backgroundGpuBusyAbove * 100.0)
            return false;
        found = true;
    }
    return found;
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
        if (GpuIsBusy(usage, thresholds))
        {
            adjustment.allowOptionalBackgroundWork = false;
            adjustment.allowOpportunisticVision = false;
            adjustment.reason = "GPU engines are busy; new background work waits for idle compute. "
                "No capacity reading is available.";
        }
        return adjustment;
    }

    const double peak = PeakCapacityPressure(usage, thresholds.ignoreUnmeasured);
    adjustment.budgetExceeded =
        PeakBudgetUtilisation(usage, thresholds.ignoreUnmeasured) > 1.0;

    std::ostringstream reason;
    const int percent = static_cast<int>(peak * 100.0);
    const bool idleGpuHeadroom = HasIdleGpuHeadroom(usage, thresholds) &&
        !GpuIsBusy(usage, thresholds);

    if (peak > thresholds.throttledAbove)
    {
        adjustment.state = LoadState::Throttled;
        // Keep allocations conservative. A percentage boundary alone cannot rule out
        // resident inference: on a 12 GiB card, 95.2% still leaves over 512 MiB free.
        adjustment.voicePrefetchFragments = 1;
        adjustment.allowPhraseAheadVoice = false;
        adjustment.allowOptionalBackgroundWork = idleGpuHeadroom;
        adjustment.allowOpportunisticVision = idleGpuHeadroom;
        if (idleGpuHeadroom)
            reason << "GPU memory is " << percent
                   << "% full; idle resident services still have working room. "
                      "Extra voice prefetch is limited.";
        else
            reason << "A device is " << percent
                   << "% full; waiting for memory headroom before starting optional work.";
    }
    else if (peak > thresholds.pressuredAbove)
    {
        adjustment.state = LoadState::Pressured;
        adjustment.voicePrefetchFragments = 2;
        adjustment.allowPhraseAheadVoice = false;
        // Resident weights and preallocated caches occupy VRAM between requests.
        // Occupancy alone must not strand those services for an entire idle session.
        adjustment.allowOptionalBackgroundWork = idleGpuHeadroom;
        adjustment.allowOpportunisticVision = true;
        if (adjustment.allowOptionalBackgroundWork)
            reason << "GPU memory is " << percent
                   << "% full, but measured GPU activity is low and working room remains; "
                      "background work can use the resident services.";
        else
            reason << "A device is " << percent
                   << "% full; background work is waiting for measured idle GPU headroom "
                      "or lower memory/CPU pressure.";
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

    // Activity controls when to start optional work, not whether memory is full.
    // Keep replies and already-running work intact while avoiding competing inference.
    if (GpuIsBusy(usage, thresholds))
    {
        adjustment.allowOptionalBackgroundWork = false;
        adjustment.allowOpportunisticVision = false;
        reason << " GPU engines are busy, so new background work waits for idle compute.";
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
