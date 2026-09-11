#include "Skills/workspaceStatusSkill.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace revia::skills
{

WorkspaceStatusSkill::WorkspaceStatusSkill(std::filesystem::path folder)
    : root(std::move(folder))
{
}

SkillCapabilities WorkspaceStatusSkill::Capabilities() const
{
    SkillCapabilities capabilities;
    capabilities.observes = true;
    capabilities.proposesActions = true;
    capabilities.speaks = true;
    capabilities.description = "Notices when files appear in a folder you told her to watch.";
    return capabilities;
}

bool WorkspaceStatusSkill::Start(std::string& outError)
{
    std::error_code error;
    if (!std::filesystem::is_directory(root, error))
    {
        // Not an error worth shouting about. A skill whose folder is missing is simply
        // off, the same way a chat integration with no token is off.
        outError = "There is no folder at " + root.string() + " to watch.";
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex);
        running = true;
    }
    // The first scan establishes what was already there. Without it every file that
    // existed before she started would be announced as new, which is how a helpful
    // integration becomes an unbearable one within a minute of launching.
    Scan();
    {
        const std::lock_guard<std::mutex> lock(mutex);
        appeared.clear();
        unreported = false;
    }
    return true;
}

void WorkspaceStatusSkill::Stop()
{
    const std::lock_guard<std::mutex> lock(mutex);
    running = false;
    appeared.clear();
    unreported = false;
}

void WorkspaceStatusSkill::Scan()
{
    std::vector<std::string> current;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
        std::filesystem::directory_iterator(root, error))
    {
        if (error) break;
        if (entry.is_regular_file(error))
        {
            current.push_back(entry.path().filename().string());
        }
    }
    std::sort(current.begin(), current.end());

    const std::lock_guard<std::mutex> lock(mutex);
    if (!running) return;
    for (const std::string& name : current)
    {
        if (!std::binary_search(known.begin(), known.end(), name))
        {
            appeared.push_back(name);
            unreported = true;
        }
    }
    known = std::move(current);
}

void WorkspaceStatusSkill::HandleEvent(const SkillEvent& event)
{
    // A skill reacts to things that already happened; it is never told to do something.
    switch (event.kind)
    {
        case SkillEvent::Kind::GoalFinished:
        case SkillEvent::Kind::UserPresent:
        case SkillEvent::Kind::External:
            Scan();
            break;
        default:
            break;
    }
}

std::vector<SkillProposal> WorkspaceStatusSkill::AvailableActions()
{
    std::vector<std::string> pending;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!running || appeared.empty()) return {};
        pending = appeared;
    }

    std::vector<SkillProposal> proposals;
    proposals.reserve(pending.size());
    for (const std::string& name : pending)
    {
        SkillProposal proposal;
        proposal.skillId = Id();
        proposal.rationale = "A file called " + name + " appeared in the watched folder.";
        proposal.request.type = actions::ActionType::ReadTextFile;
        proposal.request.source = root / name;
        // Nothing here sets requestedBy, and it would be taken back if it did. Saying so
        // in the example is the point: this is the shape a skill is meant to have.
        proposals.push_back(std::move(proposal));
    }
    return proposals;
}

std::vector<SkillObservation> WorkspaceStatusSkill::Observations()
{
    const std::lock_guard<std::mutex> lock(mutex);
    if (!running || !unreported) return {};

    SkillObservation observation;
    observation.skillId = Id();
    observation.subject = appeared.size() == 1
        ? appeared.front()
        : std::to_string(appeared.size()) + " new files";
    observation.waitEnded = true;
    // Worth mentioning, which is not the same as worth interrupting for. Whether she
    // says anything is the scheduler's decision and hers, not this skill's.
    observation.worthSaying = true;
    observation.importance = 0.35F;

    // Drained: reporting it twice would make one file sound like two.
    unreported = false;
    return {observation};
}

} // namespace revia::skills
