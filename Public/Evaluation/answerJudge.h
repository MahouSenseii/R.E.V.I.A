#pragma once

#include "Core/conversationMessage.h"

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::evaluation
{
// One request to a judge model: a system prompt, a user message carrying the evidence,
// and the JSON schema the reply must follow.
struct JudgeRequest
{
    std::string system;
    std::string user;
    std::string schema;
};

struct JudgeCompletion
{
    bool succeeded = false;
    std::string text;
    std::string error;
};

// The seam between judging and whoever runs the judge model. The live tool supplies the
// local llama.cpp tiers through messageRouter; a cloud judge is another implementation of
// this class, and nothing else changes.
class JudgeBackend
{
  public:
    virtual ~JudgeBackend() = default;
    [[nodiscard]] virtual JudgeCompletion Complete(const JudgeRequest& request, std::stop_token stopToken) const = 0;
    // Recorded with every verdict, e.g. "local:Expert:<model>".
    [[nodiscard]] virtual std::string Identity() const = 0;
};

struct JudgeInput
{
    std::string caseId;
    std::string category;
    std::string rubric;
    // Earlier turns of the same case, in order.
    std::vector<conversationMessage> priorTurns;
    std::string finalInput;
    std::string candidate;
    // A stored reply from the accepted baseline run. Present means a pairwise judgment.
    std::optional<std::string> baseline;
};

enum class JudgePreference
{
    None,
    Candidate,
    Baseline,
    Tie
};

[[nodiscard]] std::string ToString(JudgePreference preference);

struct JudgeVerdict
{
    // False when the judge could not be asked or its reply broke the schema. An unavailable
    // verdict is unjudged, never a failure.
    bool available = false;
    int score = 0;
    bool pass = false;
    std::string reason;
    bool pairwise = false;
    JudgePreference preference = JudgePreference::None;
    std::string judgeId;
    std::string error;
};

[[nodiscard]] std::string JudgeSchema(bool pairwise);
[[nodiscard]] JudgeRequest BuildJudgeRequest(const JudgeInput& input);
// Strict: exactly the schema's fields, score 1 to 5, a nonempty reason, and pass agreeing
// with the score (4 and 5 pass, 1 and 2 fail, 3 may go either way).
[[nodiscard]] bool ParseJudgeVerdict(const std::string& text, bool pairwise, JudgeVerdict& outVerdict, std::string& outError);

class AnswerJudge
{
  public:
    explicit AnswerJudge(std::shared_ptr<const JudgeBackend> backend);
    [[nodiscard]] JudgeVerdict Judge(const JudgeInput& input, std::stop_token stopToken = {}) const;
    [[nodiscard]] std::string Identity() const;

  private:
    std::shared_ptr<const JudgeBackend> backend;
};
}
