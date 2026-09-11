#include "Skills/skillManager.h"

#include <algorithm>
#include <utility>

namespace revia::skills
{

actions::ActionRequest SanitizeProposedRequest(
    const actions::ActionRequest& request, const std::string& skillId)
{
    actions::ActionRequest cleaned = request;

    // Origin is the field that decides how much trust an action gets, and it is not a
    // skill's to state. Anything a skill proposes is autonomous by construction: a
    // Discord integration saying "the user asked for this" is precisely the lie the
    // desktop authorizer must not be able to be told.
    cleaned.requestedBy = "autonomous:skill:" + skillId;

    // A skill has no eyes. Vision resolution is evidence that a model looked at the
    // screen and a user confirmed what it found; a skill asserting it would be claiming
    // a confirmation that never happened.
    cleaned.resolution = actions::ActionRequest::ElementResolutionEvidence{};

    // Minted by the runtime so audit records cannot be made to collide.
    cleaned.id = actions::NewActionId();
    return cleaned;
}

void SkillManager::SetPolicy(std::shared_ptr<policy::CapabilityPolicy> policy)
{
    const std::lock_guard<std::mutex> lock(mutex);
    capabilityPolicy = std::move(policy);
}

void SkillManager::SetProposalSink(ProposalSink value)
{
    const std::lock_guard<std::mutex> lock(mutex);
    sink = std::move(value);
}

bool SkillManager::Add(SkillHandle skill, std::string& outError)
{
    if (skill == nullptr)
    {
        outError = "No skill was supplied.";
        return false;
    }
    const std::string id = skill->Id();
    if (id.empty())
    {
        outError = "A skill must have an id.";
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const bool duplicate = std::any_of(skills.begin(), skills.end(),
            [&id](const SkillHandle& existing)
            {
                return existing != nullptr && existing->Id() == id;
            });
        if (duplicate)
        {
            outError = "A skill called " + id + " is already running.";
            return false;
        }
    }
    // Started outside the lock: an integration that opens a socket must not be able to
    // hold up every other skill while it does.
    if (!skill->Start(outError))
    {
        return false;
    }
    const std::lock_guard<std::mutex> lock(mutex);
    skills.push_back(std::move(skill));
    return true;
}

void SkillManager::Remove(const std::string& skillId)
{
    SkillHandle removed;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto found = std::find_if(skills.begin(), skills.end(),
            [&skillId](const SkillHandle& skill)
            {
                return skill != nullptr && skill->Id() == skillId;
            });
        if (found == skills.end()) return;
        removed = *found;
        skills.erase(found);
    }
    if (removed != nullptr) removed->Stop();
}

void SkillManager::StopAll()
{
    std::vector<SkillHandle> stopping;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        stopping.swap(skills);
    }
    for (const SkillHandle& skill : stopping)
    {
        if (skill != nullptr) skill->Stop();
    }
}

std::vector<std::string> SkillManager::ActiveSkills() const
{
    const std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> names;
    names.reserve(skills.size());
    for (const SkillHandle& skill : skills)
    {
        if (skill != nullptr) names.push_back(skill->Id());
    }
    return names;
}

void SkillManager::Dispatch(const SkillEvent& event)
{
    std::vector<SkillHandle> targets;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        targets = skills;
    }
    for (const SkillHandle& skill : targets)
    {
        if (skill != nullptr) skill->HandleEvent(event);
    }
}

std::vector<ProposalVerdict> SkillManager::CollectAndForward()
{
    std::vector<SkillHandle> targets;
    std::shared_ptr<policy::CapabilityPolicy> policy;
    ProposalSink forward;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        targets = skills;
        policy = capabilityPolicy;
        forward = sink;
    }

    std::vector<ProposalVerdict> verdicts;
    for (const SkillHandle& skill : targets)
    {
        if (skill == nullptr) continue;
        const std::string id = skill->Id();
        for (const SkillProposal& raw : skill->AvailableActions())
        {
            SkillProposal proposal = raw;
            proposal.skillId = id;
            proposal.request = SanitizeProposedRequest(raw.request, id);

            ProposalVerdict verdict;
            verdict.skillId = id;

            // No policy, no action. An unconfigured manager refusing everything is the
            // correct failure: the alternative is a default that quietly permits.
            if (policy == nullptr)
            {
                verdict.explanation =
                    "No capability policy is attached, so nothing a skill proposes can be "
                    "evaluated.";
                verdicts.push_back(verdict);
                continue;
            }

            verdict.decision = policy->Evaluate(proposal.request);
            if (verdict.decision.verdict != actions::PolicyVerdict::Allowed)
            {
                verdict.explanation = verdict.decision.reason;
                verdicts.push_back(verdict);
                continue;
            }
            if (!forward)
            {
                verdict.explanation = "Nothing is listening for skill proposals.";
                verdicts.push_back(verdict);
                continue;
            }
            verdict.forwarded = true;
            verdict.explanation = proposal.rationale;
            verdicts.push_back(verdict);
            // The decision travels with the request. Whoever executes still applies rate
            // limiting, desktop authorization, approval and audit; forwarding is not
            // permission, it is delivery.
            forward(proposal, verdict.decision);
        }
    }
    return verdicts;
}

void SkillManager::ContributeEvidence(autonomy::AutonomyEvidence& evidence)
{
    std::vector<SkillHandle> targets;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        targets = skills;
    }
    for (const SkillHandle& skill : targets)
    {
        if (skill == nullptr) continue;
        for (const SkillObservation& observation : skill->Observations())
        {
            // Only the fields a skill is in a position to know about. An integration can
            // report that a build finished; it cannot report that her memory needs
            // tidying or that a goal of hers is unfinished, and there is deliberately no
            // path here for it to try.
            if (observation.waitEnded) evidence.waitEnded = true;
            if (observation.repeatedFailure) evidence.repeatedFailure = true;
            if (observation.worthSaying)
            {
                evidence.somethingWorthSaying = true;
                if (evidence.subjectWorthSaying.empty())
                {
                    evidence.subjectWorthSaying = observation.subject;
                }
            }
        }
    }
}

} // namespace revia::skills
