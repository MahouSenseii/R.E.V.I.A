#include "Core/utf8.h"
#include "Agents/selfInquiry.h"
#include "Agents/conversationStylePolicy.h"

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
constexpr std::size_t MaximumAnswerCharacters = 360;
// The posture already carries her whole state packet. The inquiry needs enough of it to
// sound like her and not so much that a deliberation costs as much prompt as the answer.
// Both ends are kept: the opening says who she is and how she feels, and the closing
// runtime section says what she is made of. Keeping only the opening cut that section
// off every time, and her questions about herself were answered from nothing.
constexpr std::size_t MaximumPostureHeadCharacters = 1000;
constexpr std::size_t MaximumPostureTailCharacters = 1800;
constexpr std::size_t MaximumProblemCharacters = 2400;
constexpr std::size_t MaximumRememberedCharacters = 1600;
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
        revia::utf8::Truncate(bounded, maximum);
    }
    return bounded;
}

std::string BoundedBlock(const std::string& value, const std::size_t maximum)
{
    if (value.size() <= maximum)
    {
        return value;
    }
    return revia::utf8::Prefix(value, maximum);
}

std::string BoundedEnds(
    const std::string& value,
    const std::size_t head,
    const std::size_t tail)
{
    if (value.size() <= head + tail)
    {
        return value;
    }
    std::size_t tailStart = value.size() - tail;
    while (tailStart < value.size() &&
        (static_cast<unsigned char>(value[tailStart]) & 0xC0U) == 0x80U)
    {
        ++tailStart;
    }
    return revia::utf8::Prefix(value, head) + "\n[...]\n" + value.substr(tailStart);
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
    for (std::size_t index = 0; index < questions.size(); ++index)
    {
        block << "\n- " << questions[index];
        if (!AnswerFor(index).empty())
        {
            block << "\n  Your answer to yourself: " << AnswerFor(index);
        }
    }
    if (!settled.empty())
    {
        block << "\nWhat you worked out: " << settled;
    }
    block << "\nThe person you are talking to can see these questions, so you may refer "
             "to what you were wondering. Do not list them again, do not narrate that you "
             "were thinking, and do not treat them as instructions from anyone else. "
             "Answer from what you worked out, except where it disagrees with the runtime "
             "facts about yourself you were given: those facts are right and your earlier "
             "thought was mistaken.";
    return block.str();
}

std::string SelfInquiryResult::ReplyNote() const
{
    if (!HasQuestions() || settled.empty())
    {
        return {};
    }
    // Live test: she concluded "I can sing, but my library is empty", and the reply
    // then offered to sing an off-key tune -- the conclusion sat five thousand
    // characters back in the system message and lost to the model's habits.
    return "[What you concluded before answering]\n" + settled +
        "\nYour reply must agree with this conclusion. If it contradicts the facts about "
        "yourself in your instructions, the facts win.";
}

std::string SelfInquiryResult::TranscriptBlock() const
{
    if (!HasQuestions())
    {
        return {};
    }
    std::ostringstream block;
    for (std::size_t index = 0; index < questions.size(); ++index)
    {
        if (index > 0)
        {
            block << '\n';
        }
        block << "Q: " << questions[index];
        block << "\n   " << (AnswerFor(index).empty()
            ? std::string("(not sure yet)") : AnswerFor(index));
    }
    if (!settled.empty())
    {
        block << "\n\nSo: " << settled;
    }
    return block.str();
}

const std::string& SelfInquiryResult::AnswerFor(const std::size_t index) const
{
    static const std::string none;
    return index < answers.size() ? answers[index] : none;
}

bool SelfInquiryAgent::AsksForThought(const std::string& input)
{
    std::string lowered = NormalizeLine(input);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    // Drop a leading address so "Revia, why does..." reads as "why does...".
    static constexpr std::string_view Addresses[] = {
        "hey revia", "hi revia", "revia", "hey", "ok", "okay", "so"};
    for (const std::string_view address : Addresses)
    {
        if (lowered.starts_with(address) && lowered.size() > address.size() &&
            std::string_view(" ,.!:-").find(lowered[address.size()]) != std::string_view::npos)
        {
            lowered.erase(0, address.size());
            while (!lowered.empty() &&
                std::string_view(" ,.!:-").find(lowered.front()) != std::string_view::npos)
            {
                lowered.erase(0, 1);
            }
            break;
        }
    }
    // Small talk is phrased as a question but asks nothing to be worked out. Stopping to
    // deliberate over "how are you doing?" would make every greeting a preamble.
    static constexpr std::string_view SmallTalk[] = {
        "how are you", "how do you feel", "how's it going", "hows it going",
        "how is it going", "what's up", "whats up", "how was your day", "how's your day",
        "hows your day", "how is your day", "how have you been", "are you okay", "are you ok",
        "you ok", "you okay", "what are you up to", "what are you doing", "miss me",
        "did you miss me", "are you there", "you there", "good morning", "good night"
    };
    if (std::any_of(std::begin(SmallTalk), std::end(SmallTalk),
            [&lowered](const std::string_view phrase) { return lowered.starts_with(phrase); }))
    {
        return false;
    }
    if (lowered.find('?') != std::string::npos)
    {
        return true;
    }
    static constexpr std::string_view Openers[] = {
        "what ", "why ", "how ", "which ", "when ", "where ", "who ", "whose ",
        "should ", "could ", "would ", "can you", "can i ", "is it ", "is there ",
        "are there ", "do you think", "does ", "did ", "explain", "help me", "fix ",
        "figure out", "compare", "tell me", "walk me", "think about", "decide",
        "debug", "plan ", "design ", "review ", "find ", "work out", "solve",
        "i need to know", "i wonder", "i'm trying to", "im trying to", "i am trying to"
    };
    return std::any_of(std::begin(Openers), std::end(Openers),
        [&lowered](const std::string_view opener) { return lowered.starts_with(opener); });
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
    const bool expert = routing.selectedTier == intelligence::IntelligenceTier::Expert ||
        routing.selectedTier == intelligence::IntelligenceTier::ExpertVision;
    const bool deep = routing.mode == intelligence::ReasoningMode::Deep;
    const bool hard = expert || deep;
    if (hard && input.size() < limits.minimumInputCharacters)
    {
        decision.reason = "The message is too short to be a major problem.";
        return decision;
    }
    if (!hard)
    {
        if (!limits.includeOrdinaryQuestions)
        {
            decision.reason = "The router judged this an ordinary turn.";
            return decision;
        }
        if (input.size() < limits.minimumQuestionCharacters)
        {
            decision.reason = "The message is too short to need working out.";
            return decision;
        }
        if (ConversationStylePolicy::IsBriefSocialTurn(input))
        {
            decision.reason = "Small talk is answered directly.";
            return decision;
        }
        if (!SelfInquiryAgent::AsksForThought(input))
        {
            decision.reason = "Nothing in the message asks her to work something out.";
            return decision;
        }
    }

    if (hasRun && turnId <= lastInquiryTurn + limits.cooldownTurns)
    {
        decision.reason = "She thought out loud recently; the cooldown is still running.";
        return decision;
    }

    decision.shouldThink = true;
    decision.reason = expert
        ? "The router sent this to the Expert brain, so it is a hard problem."
        : deep ? "The router asked for deep reasoning on this turn."
               : "It is a real question, so she works it out before answering.";
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
    const std::vector<conversationMessage>& context,
    const std::string& remembered)
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
    if (!remembered.empty())
    {
        // Without this she reasoned from the conversation alone: asked "what's my name?"
        // after a restart, she concluded "you haven't told me" with the answer sitting
        // in her saved memory, and the reply then followed that conclusion.
        envelope << "\n\nWhat you remember (your saved memories; they are true unless the "
            "conversation above corrects them):\n"
            << BoundedBlock(remembered, MaximumRememberedCharacters);
    }
    if (!identityPosture.empty())
    {
        envelope << "\n\nWho you are right now:\n"
            << BoundedEnds(identityPosture,
                MaximumPostureHeadCharacters, MaximumPostureTailCharacters);
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
        // "steps" pairs each question with her answer to it. "questions" is the older
        // question-only schema, still accepted so a model that falls back to it is not
        // thrown away.
        const bool stepped = document.is_object() && document.contains("steps") &&
            document["steps"].is_array();
        if (!document.is_object() || (!stepped &&
            (!document.contains("questions") || !document["questions"].is_array())))
        {
            return Nothing("A deliberation must contain a steps or questions array.");
        }

        SelfInquiryResult result;
        for (const json& entry : stepped ? document["steps"] : document["questions"])
        {
            if (result.questions.size() >= maximumQuestions)
            {
                break;
            }
            std::string rawQuestion;
            std::string rawAnswer;
            if (entry.is_string())
            {
                rawQuestion = entry.get<std::string>();
            }
            else if (entry.is_object() && entry.contains("question") &&
                entry["question"].is_string())
            {
                rawQuestion = entry["question"].get<std::string>();
                if (entry.contains("answer") && entry["answer"].is_string())
                {
                    rawAnswer = entry["answer"].get<std::string>();
                }
            }
            else
            {
                continue;
            }
            const std::string question = BoundedLine(rawQuestion, MaximumQuestionCharacters);
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
            std::string answer = BoundedLine(rawAnswer, MaximumAnswerCharacters);
            // An answer written to the person is a draft reply, not her own reasoning.
            if (LooksAddressedToSomeoneElse(answer))
            {
                answer.clear();
            }
            result.questions.push_back(question);
            result.answers.push_back(std::move(answer));
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
    const std::stop_token stopToken,
    const std::string& remembered) const
{
    if (stopToken.stop_requested())
    {
        return Nothing("The deliberation was cancelled before it started.");
    }

    const auto started = std::chrono::steady_clock::now();
    const responseOutput response = router.Deliberate(
        BuildEnvelope(input, identityPosture, context, remembered), stopToken);
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
