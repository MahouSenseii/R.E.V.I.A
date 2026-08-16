#pragma once

#include "Runtime/affectTypes.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace revia::runtime
{

class AffectController
{
public:
    explicit AffectController(
        std::chrono::milliseconds minimumHold = std::chrono::milliseconds(1500),
        std::chrono::milliseconds decayAfter = std::chrono::seconds(45));

    AffectSnapshot Reset();
    AffectSnapshot ObserveTurn(
        const std::string& userInput,
        const std::string& response,
        bool succeeded);
    std::optional<AffectSnapshot> Tick();
    AffectSnapshot Current() const;

private:
    struct Candidate
    {
        AffectState state = AffectState::Neutral;
        float intensity = 0.25F;
        std::string reason;
    };

    static Candidate Classify(
        const std::string& userInput,
        const std::string& response,
        bool succeeded);
    AffectSnapshot Apply(Candidate candidate, std::chrono::steady_clock::time_point now);

    mutable std::mutex mutex;
    AffectSnapshot snapshot;
    std::chrono::steady_clock::time_point lastChanged = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastObserved = lastChanged;
    std::chrono::milliseconds minimumHold;
    std::chrono::milliseconds decayAfter;
};

} // namespace revia::runtime
