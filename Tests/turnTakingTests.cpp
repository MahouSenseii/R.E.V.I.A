#include "testSupport.h"

#include "Speech/speechService.h"
#include "Speech/turnTaking.h"

#include <chrono>
#include <iostream>
#include <string>

// When a thought has ended, and the sound she makes while she thinks.
namespace
{
using namespace std::chrono_literals;
using revia::speech::ContinuationWindowMs;
using revia::speech::JudgeTurnCompletion;
using revia::speech::ThinkingFillerPolicy;
using revia::speech::TurnCompletion;
using revia::tests::Check;

void TestAThoughtThatTrailsOffIsWaitedFor()
{
    const auto completion = [](const std::string& transcript)
    {
        return JudgeTurnCompletion(transcript).completion;
    };
    Check(completion("Revia, what is a mutex?") == TurnCompletion::Complete,
        "A question with its mark was not complete.");
    Check(completion("okay") == TurnCompletion::Complete, "A short whole phrase was not complete.");
    Check(completion("sing something") == TurnCompletion::Complete,
        "A short request without a full stop was not complete.");
    Check(completion("So I was thinking about the garden and") == TurnCompletion::Unfinished,
        "A sentence ending on \"and\" was not unfinished.");
    Check(completion("I wanted to ask you about, um") == TurnCompletion::Unfinished,
        "A sentence ending on a filler was not unfinished.");
    Check(completion("Well, the thing is,") == TurnCompletion::Unfinished,
        "A trailing comma was not unfinished.");
    Check(completion("I think the problem is...") == TurnCompletion::Unfinished,
        "A trailing ellipsis was not unfinished.");
    Check(completion("I was going to tell you about the thing I") == TurnCompletion::Unfinished,
        "A sentence ending on \"I\" was not unfinished.");
    Check(completion("the build fails on the second target every time") == TurnCompletion::Uncertain,
        "A longer sentence without a mark was not uncertain.");
    Check(JudgeTurnCompletion("   ").completion == TurnCompletion::Uncertain,
        "Silence was judged.");
    Check(JudgeTurnCompletion("and then the").because == "ends on \"the\"",
        "The judgement did not say which word decided it.");

    Check(ContinuationWindowMs(TurnCompletion::Complete, 350, 900) == 350 &&
        ContinuationWindowMs(TurnCompletion::Unfinished, 350, 900) == 1250 &&
        ContinuationWindowMs(TurnCompletion::Uncertain, 350, 900) == 800 &&
        ContinuationWindowMs(TurnCompletion::Unfinished, -5, -5) == 0,
        "The continuation window does not follow the judgement.");
}

void TestTheThinkingSoundIsRareAndOnlyForAQuestion()
{
    ThinkingFillerPolicy policy(45s);
    const auto start = ThinkingFillerPolicy::Clock::now();
    Check(ThinkingFillerPolicy::AsksForThought("Why does my loop print the same value every time?") &&
        ThinkingFillerPolicy::AsksForThought("Explain recursion to me") &&
        ThinkingFillerPolicy::AsksForThought("can you work out what's wrong here") &&
        !ThinkingFillerPolicy::AsksForThought("Hey.") &&
        !ThinkingFillerPolicy::AsksForThought("I prefer dark themes."),
        "The wrong turns were taken for questions worth a pause.");
    Check(!policy.Consider("Why does it crash?", false, true, 1, start),
        "A filler played for a turn she was not going to think about.");
    Check(!policy.Consider("Why does it crash?", true, false, 1, start),
        "A filler played with no voice to play it.");
    Check(!policy.Consider("Hey.", true, true, 1, start),
        "A filler played for small talk.");
    Check(policy.Consider("Why does it crash?", true, true, 1, start),
        "A filler did not play for a question she stops to think about.");
    Check(!policy.Consider("Why does it crash?", true, true, 1, start + 1s),
        "A filler played twice in one turn.");
    Check(!policy.Consider("And why is that?", true, true, 2, start + 10s),
        "A filler played again within the interval.");
    Check(policy.Consider("And why is that?", true, true, 3, start + 50s),
        "A filler did not play once the interval had passed.");

    // The filler is her own bank sound, planned through the same path as a cue in a reply.
    speechSettings settings;
    const auto plan = revia::speech::SpeechService::PlanSpeech("*hmm*", settings,
        [](revia::speech::VocalizationKind) { return revia::speech::VocalizationVerdict::Allowed; });
    Check(plan.size() == 1 && plan.front().kind == revia::speech::SegmentKind::Vocalization &&
        plan.front().vocalization == revia::speech::VocalizationKind::Hmm,
        "A lone *hmm* did not plan as the hmm clip.");
}
} // namespace

void RunTurnTakingTests()
{
    TestAThoughtThatTrailsOffIsWaitedFor();
    TestTheThinkingSoundIsRareAndOnlyForAQuestion();
    std::cout << "A thought that trails off is waited for, a finished one is answered, and the "
                 "thinking sound is rare and only for a question.\n";
}
