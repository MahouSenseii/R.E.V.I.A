#pragma once

#include "Computer/computerTypes.h"
#include "Goals/goalTypes.h"
#include "Library/structLibrary.h"
#include "Windows/desktopObserver.h"

#include <cstdint>
#include <string>

namespace revia::computer
{

// One look at the machine, turned into the typed situation every provider reasons about.
//
// This was a hundred and thirty lines inside ReviaSession, between the speech
// coordinator and the memory agent, and it had nothing to do with either. It is the
// preparation half of a decision -- take the observation, apply the perception
// exclusions, filter the candidates through the goal's own capability scope, and notice
// whether anything changed since last time -- and none of that is session lifecycle.
//
// Moving it out is not a file split. The state it needs is genuinely its own: the
// previous screen digest and the last usable observation are facts about a task in
// progress, not about a session, and they now live with the code that reads them
// instead of beside nine thousand lines that never touch them.
//
// It observes and it filters. It grants nothing: the candidate list is narrowed by the
// same CapabilityPolicy the executor uses, so a control that appears here is one the
// scope would not refuse outright -- which is a smaller claim than "permitted", and
// every action is still checked again at execution.
// Turn one look at the screen into the candidates a decision may choose from.
//
// Free rather than a member because it is where the rules live that most need testing --
// which controls a scope would refuse outright, which a nameless control has to earn its
// place with, and the one control that never becomes a candidate whatever else is true
// of it. A test can hand this an observation it wrote by hand; it cannot hand a
// DesktopObserver a screen.
[[nodiscard]] std::vector<ObservedCandidate> BuildCandidates(
    const actions::windows::DesktopObservation& screen,
    const actions::CapabilitySettings& scope,
    std::size_t maximumListedControls = 40);

class ComputerObservationBuilder
{
public:
    explicit ComputerObservationBuilder(actions::windows::DesktopObserver& observer);

    ComputerObservationBuilder(const ComputerObservationBuilder&) = delete;
    ComputerObservationBuilder& operator=(const ComputerObservationBuilder&) = delete;

    // Exactly one look, handed to whoever decides.
    //
    // Taking a second look is what this exists to prevent: the observation generation is
    // process-wide, and a provider observing for itself would invalidate a target
    // another provider had already chosen correctly.
    [[nodiscard]] ComputerTaskContext Build(
        const goals::Goal& goal,
        std::uint32_t iteration,
        const perceptionSettings& perception);

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
