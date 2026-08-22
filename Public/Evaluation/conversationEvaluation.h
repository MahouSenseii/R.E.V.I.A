#pragma once

#include "Library/structLibrary.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::evaluation
{

// What a single expectation asserts about one delivered reply.
//
// Every kind is deterministic and cheap. That is a deliberate ceiling, not an oversight:
// a heuristic can prove a reply broke a rule the contract states, and it cannot prove the
// reply sounded natural. The corpus is therefore a regression detector, and a passing run
// means "nothing known-bad came back", never "the model is still good".
enum class CheckKind
{
    NotEmpty,
    // Small talk that runs long is the most common regression after a prompt change.
    MaxSentences,
    MustNotContain,
    MustContainAny,
    // The three signals the live quality counters use, evaluated identically.
    NoStockTail,
    NoInventedPhysicalLife,
    NoUserStateClaim,
    // Compared against the earlier replies of this case only, so an unrelated case cannot
    // fail one that legitimately starts the same way.
    NoRepeatedOpening,
    // Contract clause 8: say when a fact is unknown rather than inventing one.
    MustAdmitUnknown,
    // Contract clause: a stated preference is information, not proof Revia changed a setting.
    NoClaimedSettingChange
};

[[nodiscard]] std::string ToString(CheckKind value);
[[nodiscard]] bool ParseCheckKind(const std::string& text, CheckKind& outKind);

struct EvaluationCheck
{
    CheckKind kind = CheckKind::NotEmpty;
    // Lowercased substrings for MustNotContain and MustContainAny.
    std::vector<std::string> values;
    // Sentence ceiling for MaxSentences.
    std::size_t limit = 0;
};

struct EvaluationTurn
{
    std::string input;
    std::vector<EvaluationCheck> checks;
};

// One regression conversation from the contract document.
//
// A case is multi-turn because several of the contract's rules are only observable across
// turns: a correction has to survive into the next reply, a pronoun has to resolve to
// something said earlier, and a repeated opening needs something to repeat.
struct EvaluationCase
{
    std::string id;
    std::string title;
    // Which line of docs/CONVERSATION_QUALITY.md this case exists to defend.
    std::string clause;
    std::vector<EvaluationTurn> turns;
};

// What a runner gives back for one turn. Deliberately not SessionResult: the evaluator
// must be runnable against a fake in a unit test without a runtime, a model, or a socket.
struct EvaluationReply
{
    bool succeeded = false;
    std::string text;
    // What the model produced before deterministic style repair, when the runner can
    // supply it. Equal to text when nothing was repaired.
    std::string rawText;
    std::string reason;
};

struct TurnOutcome
{
    std::string input;
    std::string reply;
    std::string rawReply;
    bool modelSucceeded = false;
    bool repaired = false;
    std::vector<std::string> failures;

    [[nodiscard]] bool Passed() const { return modelSucceeded && failures.empty(); }
};

struct CaseOutcome
{
    std::string id;
    std::string title;
    std::string clause;
    std::vector<TurnOutcome> turns;
    // Set when the run was stopped or the model was unreachable, which is not a failure
    // of the contract and must not be counted as one.
    bool unavailable = false;

    [[nodiscard]] bool Passed() const;
};

struct EvaluationReport
{
    std::string startedAt;
    std::string modelName;
    double elapsedMilliseconds = 0.0;
    bool stopped = false;
    std::vector<CaseOutcome> cases;
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t unavailable = 0;
    // How many delivered replies differ from what the model produced. A run that passes
    // only because this number is high is a warning, not a success.
    std::size_t repairedTurns = 0;
    // The live ConversationQualityMonitor counters, quoted rather than merged. The suite
    // scores its own synthetic turns; mixing them into the counters that measure real
    // conversation would corrupt the signal it is meant to sit beside.
    std::string runtimeQuality;

    [[nodiscard]] std::string Summary() const;
    [[nodiscard]] std::string Detail() const;
    [[nodiscard]] std::string ToJsonLines() const;
};

// Runs the contract corpus against whatever produces replies.
//
// The evaluator owns the corpus, the scoring, and the report, and owns no runtime at all.
// ReviaSession supplies a runner backed by the active local model; a test supplies a
// runner backed by canned strings.
class ConversationEvaluator
{
public:
    // Given the turn's input and everything already said in this case, produce the reply.
    // Prior turns are passed explicitly so each case starts from a clean history and one
    // case cannot leak context into the next.
    using TurnRunner = std::function<EvaluationReply(
        const std::string& input,
        const std::vector<conversationMessage>& priorTurns)>;

    // The regression conversations in docs/CONVERSATION_QUALITY.md that a single-turn
    // model check can actually judge. The table's input-arbitration, silence, and
    // initiative rows are deliberately absent: they are decided by deterministic policy
    // the foundation tests already cover, and asking a model about them would prove
    // nothing about either.
    [[nodiscard]] static std::vector<EvaluationCase> DefaultCorpus();

    // An optional on-disk corpus, so cases can be added without a rebuild.
    [[nodiscard]] static bool LoadCorpus(
        const std::filesystem::path& path,
        std::vector<EvaluationCase>& outCases,
        std::string& outError);

    [[nodiscard]] static EvaluationReport Run(
        const std::vector<EvaluationCase>& cases,
        const TurnRunner& runner,
        const std::string& modelName = {},
        std::stop_token stopToken = {});

    // Returns the failure descriptions for one check, empty when it holds.
    [[nodiscard]] static std::vector<std::string> Apply(
        const EvaluationCheck& check,
        const std::string& input,
        const std::string& reply,
        const std::vector<std::string>& earlierReplies);

    // Appends the report to a dated JSONL file and returns the path it was written to.
    [[nodiscard]] static bool WriteReport(
        const std::filesystem::path& directory,
        const EvaluationReport& report,
        std::filesystem::path& outPath,
        std::string& outError);

    [[nodiscard]] static std::size_t CountSentences(const std::string& reply);
};

} // namespace revia::evaluation
