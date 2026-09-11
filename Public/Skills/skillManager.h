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

// Owns the skills, and is the only thing they can talk through.
//
// The manager deliberately does not execute anything. It evaluates a proposal against
// the same CapabilityPolicy the rest of Revia uses and hands the survivors to whoever
// owns execution, which is ReviaSession. That keeps one pipeline -- policy, rate limit,
// desktop authorization, approval, audit -- rather than a second one that skills use.
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

    // Collect what the skills want, decide, and forward what survives.
    //
    // Every proposal is evaluated. A skill cannot mark its own request approved, cannot
    // name a capability it was not given, and cannot reach the sink without a policy
    // decision attached -- the decision is produced here, from the shared policy, and
    // travels with the request.
    std::vector<ProposalVerdict> CollectAndForward();

    // Turn what the skills noticed into autonomy evidence.
    //
    // Additive: this fills in the fields skills can speak to and leaves the rest of the
    // evidence exactly as the caller built it. A skill cannot manufacture an unfinished
    // goal or a memory backlog, because those are not its to know about.
    void ContributeEvidence(autonomy::AutonomyEvidence& evidence);

private:
    mutable std::mutex mutex;
    std::vector<SkillHandle> skills;
    std::shared_ptr<policy::CapabilityPolicy> capabilityPolicy;
    ProposalSink sink;
};

// Strips everything a proposal is not allowed to assert about its own authority.
//
// Exposed for testing, because "a skill cannot approve itself" is the property most
// worth being able to check directly. A skill that sets a confirmation token, claims a
// different origin, or marks itself already-approved gets those fields taken back before
// anything looks at the request.
[[nodiscard]] actions::ActionRequest SanitizeProposedRequest(
    const actions::ActionRequest& request, const std::string& skillId);

} // namespace revia::skills
