#include "testSupport.h"

#include "Autonomy/activity.h"
#include "Autonomy/activityExecution.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using revia::tests::Check;
using namespace revia::autonomy;

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// The failure this gate exists for, observed live: the raw utterance reached the search
// backend and returned a Poison album, a Beatles song and an Evanescence song, which
// were then handed to the model as grounding for the turn.
void TestADelegatedRequestIsNeverSearchedAsWritten()
{
    for (const std::string_view raw : {
        "look up what you want !",
        "look up what you want",
        "search for whatever",
        "research anything",
        "google something",
        "find out whatever you like",
        "look into it",
        "read about stuff"})
    {
        const std::string candidate(raw);
        const ResearchTopicVerdict verdict = ResolveResearchTopic(candidate);
        Check(!verdict.usable,
            "\"" + candidate + "\" was accepted as a research topic. Searching the "
            "request itself returns results about the words in it.");
        Check(verdict.delegated,
            "\"" + candidate + "\" was refused but not recognised as delegating the "
            "choice, so the caller cannot tell it apart from a malformed topic.");
        Check(!verdict.refusal.empty(),
            "\"" + candidate + "\" was refused without a reason.");
        Check(verdict.topic.empty(),
            "\"" + candidate + "\" produced a topic despite being refused.");
    }
}

void TestARealTopicSurvivesItsImperative()
{
    struct Case { std::string input; std::string expected; };
    const std::vector<Case> cases{
        {"look up tail latency in distributed systems",
         "tail latency in distributed systems"},
        {"read about the Chandrasekhar limit", "the Chandrasekhar limit"},
        {"find out about CUDA graph capture", "CUDA graph capture"},
        {"research Turing bfloat16 support", "Turing bfloat16 support"},
        {"tail latency in distributed systems", "tail latency in distributed systems"},
        {"tell me about the Antikythera mechanism", "the Antikythera mechanism"},
    };
    for (const Case& item : cases)
    {
        const ResearchTopicVerdict verdict = ResolveResearchTopic(item.input);
        Check(verdict.usable,
            "\"" + item.input + "\" was refused: " + verdict.refusal);
        Check(verdict.topic == item.expected,
            "\"" + item.input + "\" resolved to \"" + verdict.topic +
            "\" instead of \"" + item.expected + "\".");
    }
}

// The distinction that makes the gate useful rather than merely restrictive: the same
// words that delegate on their own are fine when a subject follows them.
void TestDelegationWordsAreFineWhenASubjectFollows()
{
    const ResearchTopicVerdict verdict =
        ResolveResearchTopic("look up what you want to eat in Osaka");
    Check(verdict.usable,
        "A real topic was refused for containing a delegation phrase: " +
        verdict.refusal);
    Check(Contains(verdict.topic, "Osaka"),
        "The subject was lost from the resolved topic: " + verdict.topic);
}

void TestAnEmptyRequestIsRefusedWithoutBlamingTheUser()
{
    const ResearchTopicVerdict verdict = ResolveResearchTopic("   ");
    Check(!verdict.usable, "An empty request produced a topic.");
    Check(!verdict.refusal.empty(), "An empty request was refused silently.");
}

void TestTopicChoiceTakesTheFirstUsableCandidate()
{
    const ResearchTopicVerdict verdict = ChooseResearchTopic({
        "",
        "look up whatever",
        "the Antikythera mechanism",
        "tail latency"});
    Check(verdict.usable, "No candidate was accepted: " + verdict.refusal);
    Check(verdict.topic == "the Antikythera mechanism",
        "Priority order was not respected; got \"" + verdict.topic + "\".");
}

void TestNoUsableCandidateMeansNoResearch()
{
    const ResearchTopicVerdict verdict = ChooseResearchTopic({
        "look up what you want", "whatever", ""});
    Check(!verdict.usable,
        "A topic was invented from candidates that all delegated the choice. Choosing "
        "a topic at random to satisfy a drive is the behaviour autonomy excludes.");
    Check(!verdict.refusal.empty(), "The refusal carried no reason.");

    const ResearchTopicVerdict empty = ChooseResearchTopic({});
    Check(!empty.usable, "An empty candidate list produced a topic.");
}

// Drives. Acting spends the want; declining to act does not.
void TestDoingNothingSatisfiesNothing()
{
    Check(!DriveSatisfiedBy(ActivityType::Nothing).has_value(),
        "Doing nothing spent a drive, which would make a quiet Revia steadily less "
        "motivated the longer she stayed quiet.");
}

void TestEveryRealActivitySpendsTheDriveThatMotivatedIt()
{
    struct Pair { ActivityType type; Drive drive; };
    const std::vector<Pair> pairs{
        {ActivityType::Think, Drive::Learning},
        {ActivityType::Observe, Drive::Exploration},
        {ActivityType::Research, Drive::Curiosity},
        {ActivityType::ContinueGoal, Drive::UnfinishedGoal},
        {ActivityType::OrganizeMemory, Drive::Learning},
        {ActivityType::Create, Drive::Creativity},
        {ActivityType::Speak, Drive::Social},
    };
    for (const Pair& pair : pairs)
    {
        const auto drive = DriveSatisfiedBy(pair.type);
        Check(drive.has_value(),
            ToString(pair.type) + " spends no drive, so acting on it would never reduce "
            "the wanting and she would pursue it forever.");
        Check(*drive == pair.drive,
            ToString(pair.type) + " spends " + ToString(*drive) + " instead of " +
            ToString(pair.drive) + ".");
    }
}

// Lifecycle. An interruption is not a failure, and only one of those leaves something
// worth going back to.
void TestInterruptionIsResumableAndFailureIsNot()
{
    Check(!IsTerminal(ActivityStatus::Interrupted),
        "An interrupted activity was treated as finished for good.");
    Check(IsResumable(ActivityStatus::Interrupted),
        "An interrupted activity was not resumable, so the user needing attention "
        "would permanently discard what she was doing.");
    Check(IsTerminal(ActivityStatus::Completed), "Completed was not terminal.");
    Check(IsTerminal(ActivityStatus::Failed), "Failed was not terminal.");
    Check(!IsResumable(ActivityStatus::Completed),
        "A completed activity offered to resume, which would repeat finished work.");
}

void TestTheUserOutranksEverythingExceptNothing()
{
    Activity researching;
    researching.type = ActivityType::Research;
    researching.status = ActivityStatus::Running;
    Check(researching.CanBePreemptedBy(ActivityType::Speak),
        "Research could not be preempted, so the user would wait behind a lookup.");

    Activity finished;
    finished.type = ActivityType::Research;
    finished.status = ActivityStatus::Completed;
    Check(!finished.CanBePreemptedBy(ActivityType::Speak),
        "A finished activity was preempted, which would rewrite its outcome.");
}

// Autonomous creation writes only inside Revia's own workspace. The name comes from a
// model, so it is reduced to a safe shape rather than trusted.
void TestAWorkspaceNameCannotEscapeTheWorkspace()
{
    for (const std::string_view raw : {
        "../../etc/passwd",
        "..\\..\\Windows\\System32\\config",
        "C:/Users/davis/secret.txt",
        "note/../../../outside",
        "con",
        "  ...  "})
    {
        const std::string name = WorkspaceArtifactName(std::string(raw), ".md");
        Check(!Contains(name, ".."),
            "\"" + std::string(raw) + "\" produced a traversing name: " + name);
        Check(!Contains(name, "/") && !Contains(name, "\\"),
            "\"" + std::string(raw) + "\" produced a name with a separator: " + name);
        Check(!Contains(name, ":"),
            "\"" + std::string(raw) + "\" produced a name with a drive letter: " + name);
        Check(name.size() > 3, "\"" + std::string(raw) + "\" produced an empty name.");
    }
}

void TestAWorkspaceNameStaysReadableAndBounded()
{
    const std::string name =
        WorkspaceArtifactName("Tail latency in distributed systems", ".md");
    Check(name == "tail-latency-in-distributed-systems.md",
        "A normal title was not turned into a readable filename: " + name);

    const std::string long_ = WorkspaceArtifactName(std::string(400, 'x'), ".md");
    Check(long_.size() <= 64,
        "A long title produced a filename the filesystem may refuse: " +
        std::to_string(long_.size()) + " characters.");
    Check(long_.ends_with(".md"), "The extension was lost when the name was trimmed.");

    const std::string empty = WorkspaceArtifactName("", ".md");
    Check(empty == "note.md", "An empty title did not fall back to a usable name.");
}

} // namespace

void RunActivityExecutionTests()
{
    TestADelegatedRequestIsNeverSearchedAsWritten();
    TestARealTopicSurvivesItsImperative();
    TestDelegationWordsAreFineWhenASubjectFollows();
    TestAnEmptyRequestIsRefusedWithoutBlamingTheUser();
    TestTopicChoiceTakesTheFirstUsableCandidate();
    TestNoUsableCandidateMeansNoResearch();
    TestDoingNothingSatisfiesNothing();
    TestEveryRealActivitySpendsTheDriveThatMotivatedIt();
    TestInterruptionIsResumableAndFailureIsNot();
    TestTheUserOutranksEverythingExceptNothing();
    TestAWorkspaceNameCannotEscapeTheWorkspace();
    TestAWorkspaceNameStaysReadableAndBounded();
    std::cout << "Autonomous research refuses a delegated topic instead of searching "
                 "the request, acting spends the drive that\nmotivated it, an "
                 "interruption stays resumable, and anything she makes stays inside her "
                 "own workspace.\n";
}
