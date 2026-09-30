#pragma once

#include "Perception/perceptionSettings.h"
#include "Computer/computerTypes.h"
#include "Goals/goalTypes.h"
#include "Windows/desktopObserver.h"

#include <cstdint>
#include <string>

namespace revia::computer
{

// Applies perception exclusions and filters candidates by the goal's capability scope.
// Filtering grants no permission; execution rechecks every action.
[[nodiscard]] std::vector<ObservedCandidate> BuildCandidates(const actions::windows::DesktopObservation& screen,
    const actions::CapabilitySettings& scope, std::size_t maximumListedControls = 40);

class ComputerObservationBuilder
{
public:
    explicit ComputerObservationBuilder(actions::windows::DesktopObserver& observer);

    ComputerObservationBuilder(const ComputerObservationBuilder&) = delete;
    ComputerObservationBuilder& operator=(const ComputerObservationBuilder&) = delete;

    // Takes one shared observation; another look would invalidate its targets.
    [[nodiscard]] ComputerTaskContext Build(const goals::Goal& goal, std::uint32_t iteration, const perceptionSettings& perception);

    // The last observation that was actually usable, for binding a visual target
    // against. Empty when the window in front was withheld, so an excluded window
    // cannot be reached by visual targeting -- the exclusion holds by construction
    // rather than by a second check somebody has to remember to write.
    [[nodiscard]] const actions::windows::DesktopObservation& LastObservation() const
    {
        return lastObservation;
    }

    // Forgets the previous screen, so the next iteration does not compare against a
    // task that has ended. Called when a task ends, whatever its outcome.
    void Reset();

private:
    actions::windows::DesktopObserver* observer = nullptr;
    actions::windows::DesktopObservation lastObservation;
    std::string lastObservedScreen;
};

} // namespace revia::computer
