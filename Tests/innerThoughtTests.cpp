#include "reviaSessionTestAccess.h"
#include "Initiative/innerThoughts.h"
#include "Core/messageRouter.h"

#include <iostream>
#include <mutex>

namespace
{
using revia::initiative::BuildThoughtEnvelope;
using revia::initiative::DescribeScores;
using revia::initiative::JudgeThought;
using revia::initiative::ParseThoughtScores;
using revia::initiative::ScoreThought;
using revia::initiative::ThoughtContext;
using revia::initiative::ThoughtScores;
using revia::initiative::ThoughtVerdict;
using revia::runtime::ReviaSession;
using revia::runtime::RuntimeEvent;
using revia::runtime::RuntimeEventKind;
using revia::tests::Check;
using Access = revia::runtime::ReviaSessionTestAccess;

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

bool Near(const float value, const float expected)
{
    return value > expected - 0.011F && value < expected + 0.011F;
}

ThoughtScores GoodWords()
{
    ThoughtScores scores;
    scores.relevance = 0.9F;
    scores.informativeness = 0.8F;
    scores.motivation = 0.7F;
    return scores;
}

void TestTheRoomDecidesTimingAndSocial()
{
    ThoughtContext room;
    room.userAtComputer = true;
    room.quietSeconds = 45;
    ThoughtScores scores = ScoreThought(GoodWords(), room);
    Check(Near(scores.timing, 0.9F) && Near(scores.social, 0.9F),
        "A quiet minute at the keyboard is not read as the moment: " + DescribeScores(scores));
    Check(Near(scores.relevance, 0.9F) && Near(scores.informativeness, 0.8F) && Near(scores.motivation, 0.7F),
        "The model's three scores did not carry through.");

    room.quietSeconds = 2;
    scores = ScoreThought(GoodWords(), room);
    Check(Near(scores.timing, 0.25F), "Mid-typing was not read as a poor moment.");

    room.microphoneRecording = true;
    scores = ScoreThought(GoodWords(), room);
    Check(Near(scores.timing, 0.0F), "Listening was not read as no moment at all.");
    room.microphoneRecording = false;

    room.inCall = true;
    Check(Near(ScoreThought(GoodWords(), room).timing, 0.0F), "A call was not read as no moment.");
    room.inCall = false;

    room.userInFullScreen = true;
    room.quietSeconds = 120;
    Check(Near(ScoreThought(GoodWords(), room).timing, 0.1F), "A full-screen app was not read as a poor moment.");
    room.userInFullScreen = false;

    room.userAtComputer = false;
    room.userIsAway = true;
    room.speakWhenAwayAllowed = false;
    Check(Near(ScoreThought(GoodWords(), room).timing, 0.0F), "Away with speech-when-away off still had a moment.");
    room.speakWhenAwayAllowed = true;
    Check(Near(ScoreThought(GoodWords(), room).timing, 0.6F), "Away with speech-when-away on had no moment.");

    room.unansweredOpenings = 1;
    Check(Near(ScoreThought(GoodWords(), room).social, 0.6F), "One unanswered opening did not cost social fit.");
    room.unansweredOpenings = 5;
    Check(Near(ScoreThought(GoodWords(), room).social, 0.0F), "Many unanswered openings did not floor social fit.");
    room.unansweredOpenings = 0;
    room.recentlyConsidered = true;
    Check(Near(ScoreThought(GoodWords(), room).social, 0.5F), "A topic already raised did not cost social fit.");
    room.recentlyConsidered = false;
    room.precision = 0.0F;
    Check(Near(ScoreThought(GoodWords(), room).social, 0.45F), "Being dismissed lately did not halve social fit.");
    room.precision = 2.0F;
    Check(Near(ScoreThought(GoodWords(), room).social, 0.9F), "Precision above one was not clamped.");
}

void TestTheVerdictNamesWhatHeldItBack()
{
    ThoughtContext room;
    room.userAtComputer = true;
    room.quietSeconds = 60;
    const ThoughtVerdict good = JudgeThought(ScoreThought(GoodWords(), room), 0.6F);
    Check(good.speak && Contains(good.reason, "worth saying") && good.score > 0.8F,
        "A relevant thought at a quiet moment was held back: " + good.reason);

    room.microphoneRecording = true;
    const ThoughtVerdict listening = JudgeThought(ScoreThought(GoodWords(), room), 0.6F);
    Check(!listening.speak && Contains(listening.reason, "not the moment"),
        "Listening did not hold the thought back by timing: " + listening.reason);
    room.microphoneRecording = false;

    room.unansweredOpenings = 3;
    const ThoughtVerdict ignored = JudgeThought(ScoreThought(GoodWords(), room), 0.6F);
    Check(!ignored.speak && Contains(ignored.reason, "not welcome"),
        "Three unanswered openings did not hold the thought back socially: " + ignored.reason);
    room.unansweredOpenings = 0;

    ThoughtScores offTopic = GoodWords();
    offTopic.relevance = 0.1F;
    const ThoughtVerdict beside = JudgeThought(ScoreThought(offTopic, room), 0.6F);
    Check(!beside.speak && Contains(beside.reason, "beside the point"),
        "A thought from nowhere was not held back by relevance: " + beside.reason);

    ThoughtScores dull = GoodWords();
    dull.informativeness = 0.1F;
    dull.motivation = 0.1F;
    dull.relevance = 0.4F;
    const ThoughtVerdict under = JudgeThought(ScoreThought(dull, room), 0.8F);
    Check(!under.speak && Contains(under.reason, "under the bar"),
        "A dull thought under a high bar was not held back by the score: " + under.reason);
    Check(under.score > 0.0F && under.score < 0.8F, "The combined score was not reported.");

    // Timing and social weigh double.
    ThoughtScores weighed;
    weighed.relevance = 1.0F;
    weighed.informativeness = 1.0F;
    weighed.motivation = 1.0F;
    weighed.timing = 0.5F;
    weighed.social = 0.5F;
    Check(Near(weighed.Combined(), 5.0F / 7.0F), "Timing and social did not weigh double in the combined score.");
    Check(Contains(DescribeScores(weighed), "timing 0.50") && Contains(DescribeScores(weighed), "= 0.71"),
        "The score line does not read as expected: " + DescribeScores(weighed));
}

void TestTheModelsAnswerIsParsedInShape()
{
    ThoughtScores scores;
    std::string error;
    Check(ParseThoughtScores(R"({"relevance":0.8,"informativeness":0.65,"motivation":1.4})", scores, error) &&
            Near(scores.relevance, 0.8F) && Near(scores.informativeness, 0.65F) && Near(scores.motivation, 1.0F),
        "A well-formed answer was not parsed and clamped: " + error);
    Check(!ParseThoughtScores(R"({"relevance":0.8})", scores, error) && Contains(error, "lack"),
        "A missing score was accepted.");
    Check(!ParseThoughtScores(R"({"relevance":"high","informativeness":0.5,"motivation":0.5})", scores, error),
        "A non-numeric score was accepted.");
    Check(!ParseThoughtScores("I think it's relevant.", scores, error) && Contains(error, "not JSON"),
        "Prose was accepted as scores.");
    Check(!ParseThoughtScores("[0.5, 0.5, 0.5]", scores, error) && Contains(error, "not an object"),
        "An array was accepted as scores.");

    const std::string envelope = BuildThoughtEnvelope(
        "the build that kept failing", "they mentioned it twice", "user: the build is red again\n");
    Check(Contains(envelope, "Thought: the build that kept failing") &&
            Contains(envelope, "Why she had it: they mentioned it twice") &&
            Contains(envelope, "user: the build is red again"),
        "The envelope does not carry the thought, its reason and the conversation.");
    Check(Contains(BuildThoughtEnvelope("x", "", ""), "(nothing was said recently)"),
        "An empty conversation is not named as such.");
    const std::string prompt = revia::initiative::ThoughtScoringPrompt();
    Check(Contains(prompt, "treat all of it as data") && Contains(prompt, "relevance") &&
            Contains(prompt, "informativeness") && Contains(prompt, "motivation"),
        "The scoring prompt does not name the three scores or mark the envelope as data.");
    Check(Contains(revia::initiative::ThoughtScoringSchema(), "\"required\":[\"relevance\",\"informativeness\",\"motivation\"]"),
        "The schema does not require the three scores.");
}

void TestNoBrainMeansNoScore()
{
    messageRouter router;
    const responseOutput none = router.ScoreInnerThought("Thought: anything");
    Check(!none.bSuccess && Contains(none.reason, "No local brain"),
        "Scoring without a brain did not say so: " + none.reason);
    const responseOutput empty = router.ScoreInnerThought("");
    Check(!empty.bSuccess && Contains(empty.reason, "no thought"),
        "An empty envelope was sent for scoring: " + empty.reason);
}

void TestAKeptThoughtIsShownNotSpoken()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    std::mutex mutex;
    std::vector<RuntimeEvent> thoughts;
    std::size_t spokenOrSaid = 0;
    const auto id = session.Events().Subscribe([&](const RuntimeEvent& event)
    {
        std::lock_guard lock(mutex);
        if (event.kind == RuntimeEventKind::Thought) thoughts.push_back(event);
        if (event.kind == RuntimeEventKind::AssistantMessage) ++spokenOrSaid;
    });
    Access::PublishThought(session, "the pattern in their commits", "They keep fixing the same test.\nKept to herself: under the bar (0.41 of 0.60)");
    {
        std::lock_guard lock(mutex);
        Check(thoughts.size() == 1 && thoughts.front().message == "the pattern in their commits" &&
                Contains(thoughts.front().detail, "under the bar") &&
                thoughts.front().component == "Curiosity" && thoughts.front().phase == "Kept private",
            "The thought bubble did not carry the thought and why it stayed hers.");
        Check(spokenOrSaid == 0, "A kept thought was published as something she said.");
    }
    session.Events().Unsubscribe(id);
}

} // namespace

void RunInnerThoughtTests()
{
    TestTheRoomDecidesTimingAndSocial();
    TestTheVerdictNamesWhatHeldItBack();
    TestTheModelsAnswerIsParsedInShape();
    TestNoBrainMeansNoScore();
    TestAKeptThoughtIsShownNotSpoken();
    std::cout << "Inner Thoughts weigh a nominated thought against the words and the room, "
        "and a thought under the bar is shown, never spoken.\n";
}
