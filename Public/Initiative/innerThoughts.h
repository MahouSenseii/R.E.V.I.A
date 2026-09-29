#pragma once

#include <string>

namespace revia::initiative
{

// Whether a thought of hers is worth saying out loud, right now.
//
// The curiosity planner nominates what she might say; that is content, not permission.
// Inner Thoughts (Liu et al., 2025) scores a candidate thought before it is voiced on
// five things: how it bears on what is going on (relevance), whether it adds anything
// (informativeness), whether this is a moment for it (timing), whether saying it is
// welcome here (social), and how much she wants to (motivation). The first, second and
// last are the side model's judgement of the words; timing and social are the room's,
// and the room is known deterministically: who is at the keyboard, what is recording,
// what was left unanswered. A thought under the bar stays hers -- shown on screen as a
// thought, never spoken -- so initiative is visible without interrupting.
struct ThoughtScores
{
    float relevance = 0.0F;
    float informativeness = 0.0F;
    float timing = 0.0F;
    float social = 0.0F;
    float motivation = 0.0F;

    // Timing and social weigh double: a good remark at a bad moment is a bad remark.
    [[nodiscard]] float Combined() const;
};

// What the room is like when the thought comes.
struct ThoughtContext
{
    long long quietSeconds = 0;
    bool userAtComputer = false;
    bool userInFullScreen = false;
    bool microphoneRecording = false;
    bool inCall = false;
    bool userIsAway = false;
    bool speakWhenAwayAllowed = true;
    int unansweredOpenings = 0;
    // The same topic was brought up recently.
    bool recentlyConsidered = false;
    // Proposals accepted over dismissed lately, 0..1; 1 when there is no evidence.
    float precision = 1.0F;
};

// Timing and social from the room; the model's three carried through.
[[nodiscard]] ThoughtScores ScoreThought(const ThoughtScores& modelScores, const ThoughtContext& context);

struct ThoughtVerdict
{
    bool speak = false;
    float score = 0.0F;
    // Which dimension held it back, or why it may go.
    std::string reason;
};

[[nodiscard]] ThoughtVerdict JudgeThought(const ThoughtScores& scores, float threshold);

// The scores in one line, for the thought bubble and the log.
[[nodiscard]] std::string DescribeScores(const ThoughtScores& scores);

// The side model's part: given the thought and the recent conversation, three numbers.
[[nodiscard]] const char* ThoughtScoringPrompt();
[[nodiscard]] const char* ThoughtScoringSchema();
[[nodiscard]] std::string BuildThoughtEnvelope(
    const std::string& thought,
    const std::string& rationale,
    const std::string& recentConversation);
// The model's answer as the three scores; false when it did not answer in shape.
[[nodiscard]] bool ParseThoughtScores(const std::string& response, ThoughtScores& outScores, std::string& outError);

} // namespace revia::initiative
