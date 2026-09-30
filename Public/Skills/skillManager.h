#pragma once

#include "Autonomy/activityScheduler.h"
#include "Policy/capabilityPolicy.h"
#include "Skills/reviaSkill.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace revia::skills
{

// What happened to a proposal. Returned rather than silently dropped, so a skill author
// can see why nothing happened and a user can be told.
struct ProposalVerdict
{
    std::string skillId;
    bool forwarded = false;
    // The policy's own decision, unmodified.
    actions::PolicyDecision decision;
    std::string explanation;
};

// Owns skills and evaluates proposals; the session's single pipeline executes survivors.
class SkillManager
{
public:
    // Where forwarded proposals go. Supplied by the owner, never by a skill.
    using ProposalSink = std::function<void(const SkillProposal&, const actions::PolicyDecision&)>;

    void SetPolicy(std::shared_ptr<policy::CapabilityPolicy> policy);
    void SetProposalSink(ProposalSink sink);

    bool Add(SkillHandle skill, std::string& outError);
    void Remove(const std::string& skillId);
    void StopAll();

    [[nodiscard]] std::vector<std::string> ActiveSkills() const;

    // Fan an event out to every running skill.
    void Dispatch(const SkillEvent& event);

    // Every proposal receives a shared-policy decision before forwarding.
    // Skills cannot claim approval or capabilities they were not given.
    std::vector<ProposalVerdict> CollectAndForward();

    // Adds only skill-owned evidence; preserves caller-owned goals and memory backlog.
    void ContributeEvidence(autonomy::AutonomyEvidence& evidence);

private:
    mutable std::mutex mutex;
    std::vector<SkillHandle> skills;
    std::shared_ptr<policy::CapabilityPolicy> capabilityPolicy;
    ProposalSink sink;
};

// Removes claimed confirmation, origin, and approval fields before evaluating a proposal.
[[nodiscard]] actions::ActionRequest SanitizeProposedRequest(const actions::ActionRequest& request, const std::string& skillId);

} // namespace revia::skills
