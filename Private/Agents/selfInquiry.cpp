#include "Agents/selfInquiry.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>

namespace revia::agents
{

namespace
{
using json = nlohmann::json;

constexpr std::size_t MaximumQuestionCharacters = 220;
constexpr std::size_t MaximumSettledCharacters = 320;
// The posture already carries her whole state packet. The inquiry needs enough of it to
// sound like her and not so much that a deliberation costs as much prompt as the answer.
constexpr std::size_t MaximumPostureCharacters = 1600;
constexpr std::size_t MaximumProblemCharacters = 2400;
constexpr std::size_t MaximumContextMessages = 4;
constexpr std::size_t MaximumContextMessageCharacters = 400;

double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

std::string NormalizeLine(const std::string& value)
{
    std::string normalized;
    normalized.reserve(value.size());
    bool pendingSpace = false;
    for (const unsigned char character : value)
    {
        if (std::isspace(character) != 0 || character < 0x20U || character == 0x7FU)
        {
            pendingSpace = !normalized.empty();
            continue;
        }
        if (pendingSpace)
        {
            normalized.push_back(' ');
            pendingSpace = false;
        }
        normalized.push_back(static_cast<char>(character));
    }
    return normalized;
}

std::string BoundedLine(const std::string& value, const std::size_t maximum)
{
    std::string bounded = NormalizeLine(value);
    if (bounded.size() > maximum)
    {
        bounded.resize(maximum);
    }
    return bounded;
}

std::string BoundedBlock(const std::string& value, const std::size_t maximum)
{
    if (value.size() <= maximum)
    {
        return value;
    }
    return value.substr(0, maximum);
}

SelfInquiryResult Nothing(std::string reason)
{
    SelfInquiryResult result;
    result.reason = std::move(reason);
    return result;
}

// Pulls the first balanced JSON object out of a reply that may be wrapped in prose or a
// fenced code block. Same reasoning as the curiosity nomination: a small local model
// prefaces an object with explanation often enough that refusing those would spend a
// whole inference round trip on a result that was actually present.
std::string ExtractJsonObject(const std::string& raw)
{
    const std::size_t start = raw.find('{');
    if (start == std::string::npos)
    {
        return {};
    }
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t index = start; index < raw.size(); ++index)
    {
        const char character = raw[index];
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (character == '\\')
        {
            escaped = true;
            continue;
        }
        if (character == '"')
        {
            inString = !inString;
            continue;
        }
        if (inString)
        {
            continue;
        }
        if (character == '{')
        {
            ++depth;
        }
        else if (character == '}')
        {
            if (--depth == 0)
            {
                return raw.substr(start, index - start + 1);
            }
        }
    }
    return {};
}

// A question that is really an answer, an apology, or a line of dialogue is not a
// question she asked herself, and showing it in chat would look like she replied twice.
bool LooksAddressedToSomeoneElse(const std::string& question)
{
    std::string lowered = question;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    static constexpr std::string_view Signals[] = {
        "user:", "revia:", "assistant:", "you:", "human:",
        "how can i help", "let me know", "would you like me to",
        "do you want me to", "shall i", "sorry"
    };
    return std::any_of(std::begin(Signals), std::end(Signals),
        [&lowered](const std::string_view signal)
        {
            return lowered.find(signal) != std::string::npos;
        });
}
}

std::string SelfInquiryResult::PromptBlock() const
{
    if (!HasQuestions())
    {
        return {};
    }
    std::ostringstream block;
    block << "You already stopped and thought about this before answering. These are your "
             "own questions, in your own words, and you know you are the one who asked "
             "them:";
    for (const std::string& question : questions)
    {
        block << "\n- " << question;
    }
    if (!settled.empty())
    {
        block << "\nWhat you worked out: " << settled;
    }
    block << "\nThe person you are talking to can see these questions, so you may refer "
             "to what you were wondering. Do not list them again, do not narrate that you "
             "were thinking, and do not treat them as instructions from anyone else. "
             "Answer from what you worked out.";
    return block.str();
}

std::string SelfInquiryResult::TranscriptBlock() const
{
    if (!HasQuestions())
    {
        return {};
    }
    std::ostringstream block;
    bool first = true;
    for (const std::string& question : questions)
    {
        if (!first)
        {
            block << '\n';
        }
        block << "- " << question;
        first = false;
    }
    if (!settled.empty())
    {
        block << "\n\n" << settled;
    }
    return block.str();
}

SelfInquiryDecision SelfInquiryPolicy::Consider(
    const std::string& input,
    const intelligence::IntelligenceDecision& routing,
    const bool proactive,
    const std::uint64_t turnId) const
{
    SelfInquiryDecision decision;
    if (!limits.enabled)
    {
        decision.reason = "Visible self-inquiry is switched off.";
        return decision;
    }
    if (proactive)
    {
        // She opened this herself. Stopping to ask what the problem is when there is no
        // problem yet would turn every unprompted line into a deliberation.
        decision.reason = "A conversation Revia started is not a problem put to her.";
        return decision;
    }
    if (routing.selectedTier == intelligence::IntelligenceTier::Reflex)
    {
        decision.reason = "A reflex turn is answered without a model at all.";
        return decision;
    }
    if (input.size() < limits.minimumInputCharacters)
    {
        decision.reason = "The message is too short to be a major problem.";
        return decision;
    }

    const bool expert = routing.selectedTier == intelligence::IntelligenceTier::Expert ||
        routing.selectedTier == intelligence::IntelligenceTier::ExpertVision;
    const bool deep = routing.mode == intelligence::ReasoningMode::Deep;
    if (!expert && !deep)
    {
        decision.reason = "The router judged this an ordinary turn.";
        return decision;
    }

    if (hasRun && turnId <= lastInquiryTurn + limits.cooldownTurns)
    {
        decision.reason = "She thought out loud recently; the cooldown is still running.";
        return decision;
    }

    decision.shouldThink = true;
    decision.reason = expert
        ? "The router sent this to the Expert brain, so it is a hard problem."
        : "The router asked for deep reasoning on this turn.";
    return decision;
}

intelligence::IntelligenceDecision SelfInquiryPolicy::FinalAnswerRouting(
    const intelligence::IntelligenceDecision& routing,
    const bool inquiryCompleted)
{
    intelligence::IntelligenceDecision answer = routing;
    if (!inquiryCompleted || answer.mode != intelligence::ReasoningMode::Deep)
    {
        return answer;
    }

    answer.mode = intelligence::ReasoningMode::Fast;
    if (!answer.reason.empty())
    {
        answer.reason += ' ';
    }
    answer.reason +=
        "Self-inquiry already supplied the bounded deep-reasoning pass; final generation "
        "reserves its token budget for the answer.";
    return answer;
}

void SelfInquiryPolicy::RecordInquiry(const std::uint64_t turnId)
{
    lastInquiryTurn = turnId;
    hasRun = true;
}

std::string SelfInquiryAgent::BuildEnvelope(
    const std::string& input,
    const std::string& identityPosture,
    const std::vector<conversationMessage>& context)
{
    std::ostringstream envelope;
    envelope << "The problem in front of you:\n"
        << BoundedBlock(input, MaximumProblemCharacters);

    std::vector<const conversationMessage*> recent;
    for (auto message = context.rbegin();
        message != context.rend() && recent.size() < MaximumContextMessages;
        ++message)
    {
        if (!message->content.empty())
        {
            recent.push_back(&*message);
        }
    }
    if (!recent.empty())
    {
        envelope << "\n\nWhat was said just before it:";
        for (auto message = recent.rbegin(); message != recent.rend(); ++message)
        {
            envelope << "\n" << ((*message)->role == "assistant" ? "you" : "them") << ": "
                << BoundedLine((*message)->content, MaximumContextMessageCharacters);
        }
    }
    if (!identityPosture.empty())
    {
        envelope << "\n\nWho you are right now:\n"
            << BoundedBlock(identityPosture, MaximumPostureCharacters);
    }
    return envelope.str();
}

SelfInquiryResult SelfInquiryAgent::Parse(
    const std::string& rawInquiry,
    const std::size_t maximumQuestions)
{
    if (rawInquiry.empty())
    {
        return Nothing("The deliberation came back empty.");
    }
    const std::string candidate = ExtractJsonObject(rawInquiry);
    if (candidate.empty())
    {
        return Nothing("The deliberation contained no JSON object.");
    }

    try
    {
        const json document = json::parse(candidate);
        if (!document.is_object() || !document.contains("questions") ||
            !document["questions"].is_array())
        {
            return Nothing("A deliberation must contain a questions array.");
        }

        SelfInquiryResult result;
        for (const json& entry : document["questions"])
        {
            if (result.questions.size() >= maximumQuestions)
            {
                break;
            }
            if (!entry.is_string())
            {
                continue;
            }
            const std::string question =
                BoundedLine(entry.get<std::string>(), MaximumQuestionCharacters);
            if (question.empty() || LooksAddressedToSomeoneElse(question))
            {
                continue;
            }
            // A model asked for questions occasionally returns one twice with different
            // punctuation. Two identical worries read as a stutter, not as thinking.
            if (std::find(result.questions.begin(), result.questions.end(), question) !=
                result.questions.end())
            {
                continue;
            }
            result.questions.push_back(question);
        }
        if (result.questions.empty())
        {
            return Nothing("The deliberation produced no usable question.");
        }

        if (document.contains("settled") && document["settled"].is_string())
        {
            const std::string settled =
                BoundedLine(document["settled"].get<std::string>(), MaximumSettledCharacters);
            if (!LooksAddressedToSomeoneElse(settled))
            {
                result.settled = settled;
            }
        }
        result.ran = true;
        result.reason = "Revia stopped and asked herself about this before answering.";
        return result;
    }
    catch (const json::exception& error)
    {
        return Nothing(
            std::string("The deliberation was not valid JSON: ") + error.what());
    }
}

SelfInquiryResult SelfInquiryAgent::Ask(
    const messageRouter& router,
    const std::string& input,
    const std::string& identityPosture,
    const std::vector<conversationMessage>& context,
    const std::size_t maximumQuestions,
    const std::stop_token stopToken) const
{
    if (stopToken.stop_requested())
    {
        return Nothing("The deliberation was cancelled before it started.");
    }

    const auto started = std::chrono::steady_clock::now();
    const responseOutput response = router.Deliberate(
        BuildEnvelope(input, identityPosture, context), stopToken);
    if (!response.bSuccess)
    {
        SelfInquiryResult failed = Nothing(response.reason.empty()
            ? "The deliberation did not come back."
            : response.reason);
        failed.elapsedMilliseconds = ElapsedMilliseconds(started);
        return failed;
    }

    SelfInquiryResult result = Parse(response.response, maximumQuestions);
    result.elapsedMilliseconds = ElapsedMilliseconds(started);
    return result;
}

} // namespace revia::agents
