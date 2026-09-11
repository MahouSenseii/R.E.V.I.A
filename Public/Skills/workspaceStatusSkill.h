#pragma once

#include "Skills/reviaSkill.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace revia::skills
{

// The worked example: a skill that watches one folder and notices when it changes.
//
// Chosen because it is genuinely useful and completely boring. It reads a directory
// listing and nothing else -- no network, no credentials, no window it can click. What it
// demonstrates is the shape every later integration has to fit: it observes, it reports
// what it noticed, and when it wants something done it proposes an action and waits for
// the same policy every other action faces.
//
// It deliberately proposes a ReadFile rather than anything consequential, so the example
// cannot become the thing that quietly widened what skills may do.
class WorkspaceStatusSkill : public IReviaSkill
{
public:
    explicit WorkspaceStatusSkill(std::filesystem::path folder);

    [[nodiscard]] std::string Id() const override { return "workspace-status"; }
    [[nodiscard]] SkillCapabilities Capabilities() const override;

    bool Start(std::string& outError) override;
    void Stop() override;
    void HandleEvent(const SkillEvent& event) override;
    [[nodiscard]] std::vector<SkillProposal> AvailableActions() override;
    [[nodiscard]] std::vector<SkillObservation> Observations() override;

    // Re-reads the folder. Called by HandleEvent; exposed so a test can drive the scan
    // rather than wait for a timer.
    void Scan();

private:
    mutable std::mutex mutex;
    std::filesystem::path root;
    bool running = false;
    std::vector<std::string> known;
    std::vector<std::string> appeared;
    // Set when something new showed up and she has not been told yet.
    bool unreported = false;
};

} // namespace revia::skills
