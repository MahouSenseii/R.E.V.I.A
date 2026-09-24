#include "Identity/developmentEngine.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace revia::identity
{

namespace
{
    std::string Timestamp()
    {
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm parts{};
#ifdef _WIN32
        gmtime_s(&parts, &now);
#else
        gmtime_r(&now, &parts);
#endif
        std::ostringstream stamp;
        stamp << std::put_time(&parts, "%Y-%m-%dT%H:%M:%SZ");
        return stamp.str();
    }
}

DevelopmentEngine::DevelopmentEngine(DevelopmentLimits inputLimits)
    : limits(inputLimits)
{
}

std::optional<DevelopmentChange> DevelopmentEngine::Observe(
    const DevelopmentEvidence& evidence)
{
    if (evidence.trait == Trait::Count)
    {
        return std::nullopt;
    }
    std::lock_guard lock(mutex);

    // Contradicting evidence subtracts rather than being discarded. Without this,
    // development would be a ratchet: every trait would only ever accumulate in the
    // direction it first happened to move, and a habit she had grown out of would keep
    // pulling forever.
    pending[evidence.trait] += evidence.increases ? 1.0F : -1.0F;
    ++observations[evidence.trait];
    lastReason[evidence.trait] = evidence.reason;

    const float accumulated = pending[evidence.trait];
    if (std::abs(accumulated) < limits.minimumEvidence)
    {
        // The ordinary outcome. Most observations change nothing, which is what makes a
        // change mean something when it does happen.
        return std::nullopt;
    }

    DevelopmentChange change;
    change.trait = evidence.trait;
    change.delta = accumulated > 0.0F ? limits.maximumStep : -limits.maximumStep;
    change.evidenceCount = observations[evidence.trait];
    change.reason = lastReason[evidence.trait];
    change.recordedAt = Timestamp();

    // Consumed, not cleared. Clearing would mean the next change needs a full fresh
    // batch; consuming the threshold lets sustained evidence keep producing changes at a
    // steady rate while a single burst produces exactly one.
    pending[evidence.trait] -= change.delta > 0.0F
        ? limits.minimumEvidence
        : -limits.minimumEvidence;
    observations[evidence.trait] = 0;
    return change;
}

float DevelopmentEngine::PendingEvidence(const Trait trait) const
{
    std::lock_guard lock(mutex);
    const auto found = pending.find(trait);
    return found == pending.end() ? 0.0F : found->second;
}

DevelopmentState DevelopmentEngine::Apply(
    DevelopmentState development,
    const DevelopmentChange& change,
    const DevelopmentLimits& limits)
{
    if (change.trait == Trait::Count)
    {
        return development;
    }
    const float proposed = development.delta[change.trait] + change.delta;
    // Lifetime drift cap. She can change substantially over months and still be
    // recognisably herself; without this, sustained evidence would eventually push a
    // trait to its limit and hold it there.
    development.delta[change.trait] =
        std::clamp(proposed, -limits.maximumDrift, limits.maximumDrift);
    return development;
}

void DevelopmentEngine::Reset()
{
    std::lock_guard lock(mutex);
    pending.clear();
    observations.clear();
    lastReason.clear();
}

std::vector<DevelopmentEvidence> ReadDevelopmentEvidence(
    const TurnObservation& observation)
{
    std::vector<DevelopmentEvidence> evidence;

    // Solving something herself, and it working. Repeated often enough this is what
    // confidence and independence are actually made of.
    if (observation.actedIndependently && observation.succeeded)
    {
        evidence.push_back({Trait::Confidence, true,
            "problems she took on herself kept working out"});
        evidence.push_back({Trait::Independence, true,
            "she has been solving things without being walked through them"});
    }
    // Acting before checking, and it not working. Note the direction is not assumed:
    // impulsiveness only falls when impulsive attempts actually fail.
    if (observation.actedImpulsively && !observation.succeeded)
    {
        evidence.push_back({Trait::Impulsiveness, false,
            "acting before checking kept going wrong"});
        evidence.push_back({Trait::Caution, true,
            "she has been caught out by not checking first"});
    }
    // And impulsiveness that pays off pushes the other way, which is what keeps this
    // from being a one-way slide into caution.
    if (observation.actedImpulsively && observation.succeeded)
    {
        evidence.push_back({Trait::Impulsiveness, true,
            "moving quickly has been working out for her"});
        evidence.push_back({Trait::RiskTolerance, true,
            "taking a chance has been paying off"});
    }
    if (observation.wasCorrected)
    {
        evidence.push_back({Trait::Patience, true,
            "she has had to slow down and be corrected"});
    }
    if (observation.wasSocialAndPositive)
    {
        evidence.push_back({Trait::Sociability, true,
            "conversation has been going well"});
    }
    if (observation.followedCuriosity)
    {
        evidence.push_back({Trait::Curiosity, true,
            "she has been chasing things she wanted to know about"});
    }
    if (!observation.succeeded && !observation.actedImpulsively)
    {
        // Repeated failure that was not her rushing. Erodes confidence rather than
        // impulsiveness -- the distinction is what stops every setback meaning the same
        // thing.
        evidence.push_back({Trait::Confidence, false,
            "several attempts have not worked out"});
    }
    return evidence;
}

} // namespace revia::identity
