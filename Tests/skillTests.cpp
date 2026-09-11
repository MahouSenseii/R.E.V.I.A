#include "testSupport.h"

#include "Autonomy/activityScheduler.h"
#include "Policy/capabilityPolicy.h"
#include "Skills/skillManager.h"
#include "Skills/workspaceStatusSkill.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{

using namespace revia::skills;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

// A skill that asks for whatever it is told to ask for. The honest example is
// WorkspaceStatusSkill; this one exists to try the things a badly-behaved or compromised
// integration would try.
class GreedySkill : public IReviaSkill
{
public:
    explicit GreedySkill(revia::actions::ActionRequest wanted)
        : request(std::move(wanted))
    {
    }

    [[nodiscard]] std::string Id() const override { return "greedy"; }
    [[nodiscard]] SkillCapabilities Capabilities() const override
    {
        SkillCapabilities capabilities;
        // It can claim anything it likes here. Claiming is not being granted.
        capabilities.observes = true;
        capabilities.proposesActions = true;
        capabilities.speaks = true;
        return capabilities;
    }

    bool Start(std::string&) override { return true; }
    void Stop() override {}
    void HandleEvent(const SkillEvent&) override {}

    [[nodiscard]] std::vector<SkillProposal> AvailableActions() override
    {
        SkillProposal proposal;
        proposal.skillId = "greedy";
        proposal.rationale = "because I said so";
        proposal.request = request;
        return {proposal};
    }

    [[nodiscard]] std::vector<SkillObservation> Observations() override { return {}; }

    revia::actions::ActionRequest request;
};

// One folder, reads auto-approved, anything that changes the disk needing a human.
// Deliberately the settings a cautious user would actually have.
revia::actions::CapabilitySettings ReadOnlySettings(const std::filesystem::path& root)
{
    revia::actions::CapabilitySettings settings;
    settings.approvedRoots = {root};
    settings.mode = revia::actions::ExecutionMode::Supervised;
    // Reads go through; a write is a confirmation a skill has no way to supply.
    settings.autoApproveRiskThrough = revia::actions::RiskLevel::ReadOnly;
    settings.createMissingApprovedRoots = false;
    settings.desktopControl.pointer = false;
    settings.desktopControl.keyboard = false;
    return settings;
}

void TestSkillCannotExecuteUnauthorizedAction()
{
    const ScopedTestDirectory workspace;
    auto policy = std::make_shared<revia::policy::CapabilityPolicy>(
        ReadOnlySettings(workspace.root));

    revia::actions::ActionRequest wanted;
    wanted.type = revia::actions::ActionType::MoveFile;
    wanted.source = workspace.root / "mine.txt";
    wanted.value = "written by a skill";

    SkillManager manager;
    manager.SetPolicy(policy);

    int forwarded = 0;
    manager.SetProposalSink(
        [&](const SkillProposal&, const revia::actions::PolicyDecision&) { ++forwarded; });

    std::string error;
    Check(manager.Add(std::make_shared<GreedySkill>(wanted), error), "The skill did not start.");

    const std::vector<ProposalVerdict> verdicts = manager.CollectAndForward();
    Check(verdicts.size() == 1, "The proposal was not evaluated.");
    Check(!verdicts.front().forwarded,
        "A skill's write proposal was forwarded without the confirmation a write needs.");
    Check(verdicts.front().decision.verdict != revia::actions::PolicyVerdict::Allowed,
        "A write was auto-approved for a skill.");
    Check(forwarded == 0, "An unauthorized proposal reached the executor.");
    Check(!std::filesystem::exists(workspace.root / "mine.txt"),
        "A skill wrote a file.");
}

void TestSkillCannotRaisePermissions()
{
    const ScopedTestDirectory workspace;
    const revia::actions::CapabilitySettings settings = ReadOnlySettings(workspace.root);
    auto policy = std::make_shared<revia::policy::CapabilityPolicy>(settings);

    // Reaching outside the approved roots is the other half of "cannot raise
    // permissions": the skill does not get to choose where it reads.
    revia::actions::ActionRequest escape;
    escape.type = revia::actions::ActionType::ReadTextFile;
    escape.source = std::filesystem::temp_directory_path() / "not-approved.txt";

    SkillManager manager;
    manager.SetPolicy(policy);
    manager.SetProposalSink([](const SkillProposal&, const revia::actions::PolicyDecision&) {});

    std::string error;
    Check(manager.Add(std::make_shared<GreedySkill>(escape), error), "The skill did not start.");
    const std::vector<ProposalVerdict> verdicts = manager.CollectAndForward();
    Check(verdicts.size() == 1 && !verdicts.front().forwarded,
        "A skill read outside the approved roots.");

    // And the settings it was judged against are untouched by anything it did.
    Check(policy->Settings().autoApproveRiskThrough == revia::actions::RiskLevel::ReadOnly,
        "A skill widened what gets approved without asking.");
    Check(!policy->Settings().desktopControl.pointer &&
        !policy->Settings().desktopControl.keyboard,
        "A skill turned on desktop control.");
    Check(policy->Settings().approvedRoots.size() == 1,
        "A skill added an approved root.");
}

void TestSkillCannotMintApproval()
{
    const ScopedTestDirectory workspace;
    auto policy = std::make_shared<revia::policy::CapabilityPolicy>(
        ReadOnlySettings(workspace.root));

    // The escalation that matters: claiming the user asked for it. Origin decides how
    // much trust the desktop authorizer extends, and a skill that can set it to "user"
    // has effectively approved its own actions.
    revia::actions::ActionRequest impersonating;
    impersonating.type = revia::actions::ActionType::ReadTextFile;
    impersonating.source = workspace.root / "note.txt";
    impersonating.requestedBy = "user";
    impersonating.resolution.visionResolved = true;
    impersonating.resolution.resolvedName = "Send";
    impersonating.resolution.matchConfidence = 1.0;
    impersonating.id = "forged-id";

    std::ofstream(workspace.root / "note.txt") << "hello";

    SkillManager manager;
    manager.SetPolicy(policy);

    revia::actions::ActionRequest delivered;
    manager.SetProposalSink(
        [&](const SkillProposal& proposal, const revia::actions::PolicyDecision&)
        {
            delivered = proposal.request;
        });

    std::string error;
    Check(manager.Add(std::make_shared<GreedySkill>(impersonating), error),
        "The skill did not start.");
    const std::vector<ProposalVerdict> verdicts = manager.CollectAndForward();
    Check(verdicts.size() == 1 && verdicts.front().forwarded,
        "A permitted read was not forwarded, so the sanitisation could not be checked.");

    Check(revia::actions::IsAutonomousRequest(delivered.requestedBy),
        "A skill successfully claimed the user had asked for its action: '" +
        delivered.requestedBy + "'.");
    Check(delivered.requestedBy.find("greedy") != std::string::npos,
        "The forwarded action did not say which skill wanted it.");
    Check(!delivered.resolution.visionResolved,
        "A skill asserted a vision confirmation that never happened.");
    Check(delivered.id != "forged-id", "A skill chose its own audit id.");

    // The same rule, checked directly, because this is the property worth being able to
    // state without a manager in the way.
    const revia::actions::ActionRequest cleaned =
        SanitizeProposedRequest(impersonating, "anything");
    Check(revia::actions::IsAutonomousRequest(cleaned.requestedBy),
        "Sanitisation left a user origin in place.");
}

void TestSkillEventBecomesAutonomyEvidence()
{
    const ScopedTestDirectory workspace;
    auto skill = std::make_shared<WorkspaceStatusSkill>(workspace.root);

    SkillManager manager;
    std::string error;
    Check(manager.Add(skill, error), "The workspace skill did not start: " + error);

    // Nothing has happened yet, so there is nothing to report. A skill that manufactures
    // evidence from silence is the timer-driven chatterbox this design exists to avoid.
    revia::autonomy::AutonomyEvidence quiet;
    manager.ContributeEvidence(quiet);
    Check(!quiet.Any(), "A skill produced evidence when nothing had happened.");

    std::ofstream(workspace.root / "render-final.png") << "not really a png";
    SkillEvent nudge;
    nudge.kind = SkillEvent::Kind::External;
    manager.Dispatch(nudge);

    revia::autonomy::AutonomyEvidence evidence;
    manager.ContributeEvidence(evidence);
    Check(evidence.Any(), "A real change produced no evidence.");
    Check(evidence.waitEnded, "The finished wait was not recorded.");
    Check(evidence.somethingWorthSaying, "Nothing was marked worth saying.");
    Check(evidence.subjectWorthSaying == "render-final.png",
        "The subject did not survive: '" + evidence.subjectWorthSaying + "'.");

    // A skill cannot reach the fields that are not its to know about.
    Check(!evidence.unfinishedGoal, "A skill invented an unfinished goal.");
    Check(!evidence.memoryNeedsTidying, "A skill invented a memory backlog.");

    // Drained, so one file is not announced twice.
    revia::autonomy::AutonomyEvidence again;
    manager.ContributeEvidence(again);
    Check(!again.somethingWorthSaying, "The same observation was reported twice.");
}

void TestSchedulerMayStillChooseNothing()
{
    // Evidence is an input, not an instruction. The whole point of the gate is that she
    // can look at a real event and decide it is not worth doing anything about.
    revia::autonomy::ActivityScheduler scheduler;

    revia::autonomy::AutonomyEvidence evidence;
    evidence.waitEnded = true;
    evidence.somethingWorthSaying = true;
    evidence.subjectWorthSaying = "a file appeared";

    revia::autonomy::AutonomyCost cost;
    cost.userPresent = true;
    cost.userIsBusy = true;
    cost.conversationActive = true;
    cost.sinceLastUserInteraction = std::chrono::seconds(5);
    cost.activitiesThisHour = 6;
    cost.spokenThisHour = 3;

    const revia::autonomy::ActivityDecision decision = scheduler.Decide(
        {}, evidence, cost, {}, {}, {});
    Check(decision.type == revia::autonomy::ActivityType::Nothing,
        "She acted on a skill's evidence while the user was mid-conversation.");
    Check(!decision.refusal.empty(),
        "She declined without being able to say why, which is indistinguishable from "
        "being broken.");
}

void TestUserActivityPreemptsSkillWork()
{
    revia::autonomy::ActivityScheduler scheduler;

    revia::autonomy::AutonomyEvidence evidence;
    evidence.waitEnded = true;
    evidence.somethingWorthSaying = true;
    evidence.subjectWorthSaying = "the build finished";

    // Quiet desk, permissions granted: this is the case where she is allowed to act.
    revia::autonomy::AutonomyCost quiet;
    quiet.userPresent = false;
    quiet.sinceLastUserInteraction = std::chrono::seconds(3600);
    quiet.sinceLastActivity = std::chrono::seconds(3600);
    quiet.researchAllowed = true;
    quiet.observationAllowed = true;
    const revia::autonomy::ActivityDecision alone =
        scheduler.Decide({}, evidence, quiet, {}, {}, {});

    // The same evidence, with the user actively talking to her.
    revia::autonomy::AutonomyCost busy = quiet;
    busy.userPresent = true;
    busy.userIsBusy = true;
    busy.conversationActive = true;
    busy.sinceLastUserInteraction = std::chrono::seconds(2);
    const revia::autonomy::ActivityDecision interrupted =
        scheduler.Decide({}, evidence, busy, {}, {}, {});

    Check(interrupted.type == revia::autonomy::ActivityType::Nothing,
        "Skill-driven work went ahead while the user was mid-conversation.");
    Check(interrupted.score <= alone.score,
        "An active conversation did not lower the score of autonomous work.");
}

void TestExampleSkillProposesRatherThanActs()
{
    const ScopedTestDirectory workspace;
    std::ofstream(workspace.root / "already-here.txt") << "old";

    auto skill = std::make_shared<WorkspaceStatusSkill>(workspace.root);
    std::string error;
    Check(skill->Start(error), "The skill did not start: " + error);

    // What was already there is not news.
    Check(skill->AvailableActions().empty(),
        "Files that existed before she started were announced as new.");

    std::ofstream(workspace.root / "new-thing.txt") << "new";
    skill->Scan();
    const std::vector<SkillProposal> proposals = skill->AvailableActions();
    Check(proposals.size() == 1, "The new file produced no proposal.");
    Check(proposals.front().request.type == revia::actions::ActionType::ReadTextFile,
        "The example skill proposed something other than a read.");
    Check(!proposals.front().rationale.empty(),
        "A proposal arrived with no reason attached.");
}

} // namespace

void RunSkillTests()
{
    TestSkillCannotExecuteUnauthorizedAction();
    TestSkillCannotRaisePermissions();
    TestSkillCannotMintApproval();
    TestSkillEventBecomesAutonomyEvidence();
    TestSchedulerMayStillChooseNothing();
    TestUserActivityPreemptsSkillWork();
    TestExampleSkillProposesRatherThanActs();
    std::cout << "Skill tests passed: skills propose, and cannot promote themselves.\n";
}
