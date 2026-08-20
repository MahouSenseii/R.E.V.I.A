#include "Initiative/attentionPolicy.h"

#include <algorithm>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::initiative
{

AttentionContext SampleDesktop()
{
    AttentionContext context;
    context.now = std::chrono::system_clock::now();
#ifdef _WIN32
    LASTINPUTINFO lastInput{};
    lastInput.cbSize = sizeof(lastInput);
    if (GetLastInputInfo(&lastInput))
    {
        const DWORD idleMs = GetTickCount() - lastInput.dwTime;
        context.sinceLastInput = std::chrono::seconds(idleMs / 1000);
    }

    const HWND foreground = GetForegroundWindow();
    if (foreground != nullptr)
    {
        // Full screen means covering its whole monitor, which is what a game, a
        // presentation, and a shared screen all look like.
        RECT windowRect{};
        const HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetWindowRect(foreground, &windowRect) &&
            monitor != nullptr &&
            GetMonitorInfoW(monitor, &monitorInfo))
        {
            context.foregroundIsFullScreen =
                windowRect.left <= monitorInfo.rcMonitor.left &&
                windowRect.top <= monitorInfo.rcMonitor.top &&
                windowRect.right >= monitorInfo.rcMonitor.right &&
                windowRect.bottom >= monitorInfo.rcMonitor.bottom;
        }
    }
#endif
    return context;
}

std::string ToString(const AttentionVerdict value)
{
    switch (value)
    {
        case AttentionVerdict::Speak: return "speak";
        case AttentionVerdict::BelowConfidence: return "below confidence";
        case AttentionVerdict::Disabled: return "disabled";
        case AttentionVerdict::Cooldown: return "cooldown";
        case AttentionVerdict::DismissalCooldown: return "dismissal cooldown";
        case AttentionVerdict::HourlyBudget: return "hourly budget";
        case AttentionVerdict::ReducedForPrecision: return "rate reduced for low precision";
        case AttentionVerdict::UserIsBusy: return "user is mid-input";
        case AttentionVerdict::FullScreen: return "full-screen application";
        case AttentionVerdict::ExcludedApplication: return "excluded application";
    }
    return "suppressed";
}

bool IsSuppression(const AttentionVerdict value)
{
    return value != AttentionVerdict::Speak;
}

AttentionPolicy::AttentionPolicy(initiativeSettings settings)
    : configuration(std::move(settings))
{
}

float AttentionPolicy::Precision() const
{
    const std::uint32_t judged = counters.accepted + counters.dismissed;
    if (judged < static_cast<std::uint32_t>(std::max(1, configuration.precisionSampleFloor)))
    {
        // Not enough evidence to conclude anything. Treated as perfect so a single early
        // dismissal cannot mute Revia before it has had a fair chance.
        return 1.0f;
    }
    return static_cast<float>(counters.accepted) / static_cast<float>(judged);
}

bool AttentionPolicy::IsRateReduced() const
{
    return Precision() < configuration.minimumPrecision;
}

int AttentionPolicy::EffectiveHourlyBudget() const
{
    const int configured = std::max(0, configuration.maxUtterancesPerHour);
    if (!IsRateReduced())
    {
        return configured;
    }
    // Halved, floor of one: Revia keeps a way back if its proposals improve, but at a
    // rate the user is unlikely to notice as an imposition.
    return std::max(1, configured / 2);
}

AttentionVerdict AttentionPolicy::Evaluate(
    const float confidence,
    const AttentionContext& context) const
{
    if (!configuration.bEnabled)
    {
        return AttentionVerdict::Disabled;
    }
    // Confidence first. Everything below is about whether now is a good moment; this is
    // about whether the thing is worth saying at all, and it is the one the roadmap
    // insists on: a confidence threshold, not a relevance one.
    if (confidence < configuration.minimumConfidence)
    {
        return AttentionVerdict::BelowConfidence;
    }

    // Hard suppressions. These are not weighed against anything.
    if (context.foregroundIsExcluded)
    {
        return AttentionVerdict::ExcludedApplication;
    }
    if (configuration.bSuppressWhenFullScreen && context.foregroundIsFullScreen)
    {
        return AttentionVerdict::FullScreen;
    }
    if (context.sinceLastInput < std::chrono::seconds(configuration.quietInputSeconds))
    {
        return AttentionVerdict::UserIsBusy;
    }

    if (hasDismissal &&
        context.now - lastDismissed <
            std::chrono::seconds(configuration.dismissalCooldownSeconds))
    {
        return AttentionVerdict::DismissalCooldown;
    }
    if (hasSpoken &&
        context.now - lastSpoken < std::chrono::seconds(configuration.cooldownSeconds))
    {
        return AttentionVerdict::Cooldown;
    }

    const bool withinHour = hasSpoken && context.now - hourStarted < std::chrono::hours(1);
    const std::uint32_t used = withinHour ? spokenThisHour : 0;
    if (used >= static_cast<std::uint32_t>(EffectiveHourlyBudget()))
    {
        return IsRateReduced()
            ? AttentionVerdict::ReducedForPrecision
            : AttentionVerdict::HourlyBudget;
    }
    return AttentionVerdict::Speak;
}

void AttentionPolicy::RecordSpoken(const std::chrono::system_clock::time_point when)
{
    if (!hasSpoken || when - hourStarted >= std::chrono::hours(1))
    {
        hourStarted = when;
        spokenThisHour = 0;
    }
    ++spokenThisHour;
    ++counters.spoken;
    lastSpoken = when;
    hasSpoken = true;
}

void AttentionPolicy::RecordAccepted()
{
    ++counters.accepted;
}

void AttentionPolicy::RecordDismissed(const std::chrono::system_clock::time_point when)
{
    ++counters.dismissed;
    lastDismissed = when;
    hasDismissal = true;
}

void AttentionPolicy::RecordSuppressed()
{
    ++counters.suppressed;
}

InitiativeCounters AttentionPolicy::Counters() const
{
    return counters;
}

} // namespace revia::initiative
