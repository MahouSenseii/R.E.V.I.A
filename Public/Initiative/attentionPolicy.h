#pragma once

#include "Library/structLibrary.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace revia::initiative
{

// Why Revia did or did not speak. Every refusal names itself so a quiet assistant is
// diagnosable rather than merely quiet.
enum class AttentionVerdict
{
    Speak,
    BelowConfidence,
    Disabled,
    Cooldown,
    DismissalCooldown,
    HourlyBudget,
    ReducedForPrecision,
    UserIsBusy,
    FullScreen,
    ExcludedApplication
};

[[nodiscard]] std::string ToString(AttentionVerdict value);
[[nodiscard]] bool IsSuppression(AttentionVerdict value);

struct AttentionContext;

// Reads how busy the desktop looks right now. Idle time comes from GetLastInputInfo,
// which reports only *when* the last input happened and never what it was, so this needs
// no keyboard hook and creates no keylogging surface.
[[nodiscard]] AttentionContext SampleDesktop(
    const perceptionSettings& perceptionConfiguration);

// What the desktop looks like right now. Gathered by the caller so the policy itself
// stays pure and testable without a live desktop.
struct AttentionContext
{
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    // Time since any keyboard or mouse input. Sourced from GetLastInputInfo, which needs
    // no hook and reveals nothing about what was typed.
    std::chrono::seconds sinceLastInput{3600};
    bool foregroundIsFullScreen = false;
    bool foregroundIsExcluded = false;
};

struct InitiativeCounters
{
    std::uint32_t spoken = 0;
    std::uint32_t accepted = 0;
    std::uint32_t dismissed = 0;
    std::uint32_t suppressed = 0;
};

// The deterministic gate in front of every unprompted utterance.
//
// This is policy, not a prompt. A model deciding when it is welcome to interrupt is the
// same category of mistake as a model deciding its own capability scope, so the decision
// is made here on observable state and past outcomes, and the model only ever supplies
// the content and a confidence.
class AttentionPolicy
{
public:
    AttentionPolicy() = default;
    explicit AttentionPolicy(initiativeSettings settings);
    // Live comfort changes must not erase cooldowns, dismissals, or precision history.
    void UpdateSettings(initiativeSettings settings);

    [[nodiscard]] AttentionVerdict Evaluate(
        float confidence,
        const AttentionContext& context) const;

    void RecordSpoken(std::chrono::system_clock::time_point when);
    void RecordAccepted();
    void RecordDismissed(std::chrono::system_clock::time_point when);
    void RecordSuppressed();

    // Accepted over judged. Returns 1.0 until enough proposals have been judged to make
    // the ratio mean anything, so Revia is not silenced by its first dismissal.
    [[nodiscard]] float Precision() const;
    [[nodiscard]] bool IsRateReduced() const;
    [[nodiscard]] InitiativeCounters Counters() const;
    [[nodiscard]] int EffectiveHourlyBudget() const;

private:
    initiativeSettings configuration;
    InitiativeCounters counters;
    std::chrono::system_clock::time_point lastSpoken{};
    std::chrono::system_clock::time_point lastDismissed{};
    std::chrono::system_clock::time_point hourStarted{};
    std::uint32_t spokenThisHour = 0;
    bool hasSpoken = false;
    bool hasDismissal = false;
};

} // namespace revia::initiative
