#include "Agents/investigationAgent.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace revia::agents
{

namespace
{

std::string Trim(const std::string& value)
{
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<std::string> SplitFields(const std::string& line, const char separator)
{
    std::vector<std::string> fields;
    std::istringstream stream(line);
    std::string field;
    while (std::getline(stream, field, separator)) fields.push_back(Trim(field));
    return fields;
}

bool StartsWith(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

CheckKind ParseCheckKind(const std::string& value)
{
    std::string lowered;
    for (const unsigned char character : value)
    {
        lowered.push_back(static_cast<char>(std::tolower(character)));
    }
    if (lowered.find("config") != std::string::npos) return CheckKind::ResolvedConfiguration;
    if (lowered.find("source") != std::string::npos ||
        lowered.find("code") != std::string::npos) return CheckKind::SourceCode;
    if (lowered.find("log") != std::string::npos ||
        lowered.find("measure") != std::string::npos) return CheckKind::LogsAndMeasurements;
    if (lowered.find("test") != std::string::npos) return CheckKind::Tests;
    if (lowered.find("file") != std::string::npos ||
        lowered.find("state") != std::string::npos) return CheckKind::FileOrApplicationState;
    if (lowered.find("research") != std::string::npos ||
        lowered.find("web") != std::string::npos) return CheckKind::Research;
    if (lowered.find("calc") != std::string::npos ||
        lowered.find("arith") != std::string::npos) return CheckKind::Calculation;
    return CheckKind::ModelReasoning;
}

QuestionStatus ParseStatus(const std::string& value)
{
    std::string lowered;
    for (const unsigned char character : value)
    {
        lowered.push_back(static_cast<char>(std::tolower(character)));
    }
    if (lowered.find("support") != std::string::npos) return QuestionStatus::Supported;
    if (lowered.find("refut") != std::string::npos ||
        lowered.find("contradict") != std::string::npos) return QuestionStatus::Refuted;
    if (lowered.find("block") != std::string::npos) return QuestionStatus::Blocked;
    return QuestionStatus::Unresolved;
}

// Matches a question by id, or failing that by a distinctive prefix of its text. The
// model is not reliable at echoing identifiers, and a finding attached to the wrong
// question is worse than one attached to none.
const InvestigationQuestion* Resolve(
    const RoundRequest& request, const std::string& reference)
{
    const std::string trimmed = Trim(reference);
    if (trimmed.empty()) return nullptr;
    for (const InvestigationQuestion& question : request.questions)
    {
        if (question.id == trimmed) return &question;
    }
    std::string lowered;
    for (const unsigned char character : trimmed)
    {
        lowered.push_back(static_cast<char>(std::tolower(character)));
    }
    for (const InvestigationQuestion& question : request.questions)
    {
        std::string questionLowered;
        for (const unsigned char character : question.text)
        {
            questionLowered.push_back(static_cast<char>(std::tolower(character)));
        }
        if (questionLowered.find(lowered) != std::string::npos ||
            lowered.find(questionLowered) != std::string::npos)
        {
            return &question;
        }
    }
    return nullptr;
}

} // namespace

std::string InvestigationAgent::BuildOpeningEnvelope(
    const std::string& goal,
    const std::string& identityPosture,
    const std::vector<conversationMessage>& context,
    const std::size_t maximumQuestions)
{
    std::ostringstream envelope;
    envelope << identityPosture << "\n\n";
    envelope << "You are about to work on this:\n" << goal << "\n\n";
    if (!context.empty())
    {
        envelope << "Recent conversation, for context only:\n";
        const std::size_t take = std::min<std::size_t>(context.size(), 6);
        for (std::size_t i = context.size() - take; i < context.size(); ++i)
        {
            envelope << "- " << context[i].content.substr(0, 200) << "\n";
        }
        envelope << "\n";
    }
    envelope
        << "Before answering, name the questions you would actually need settled to be "
           "confident. Ask what could change the answer, not what is merely interesting, "
           "and prefer the smallest check that would settle each one.\n\n"
           "Write at most " << maximumQuestions << " lines, each exactly:\n"
           "ASK <materiality 0.0-1.0> | <the question>\n\n"
           "Nothing else. No preamble, no answer to the question itself.";
    return envelope.str();
}

std::vector<ProposedQuestion> InvestigationAgent::ParseOpeningQuestions(
    const std::string& raw, const std::size_t maximumQuestions)
{
    std::vector<ProposedQuestion> questions;
    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line) && questions.size() < maximumQuestions)
    {
        line = Trim(line);
        if (!StartsWith(line, "ASK")) continue;
        const std::vector<std::string> fields = SplitFields(line.substr(3), '|');
        if (fields.size() < 2) continue;

        // Same defence as the follow-up parse: identify the fields by what they are.
        ProposedQuestion question;
        question.materiality = 0.5;
        bool foundScore = false;
        std::string bestText;
        for (const std::string& field : fields)
        {
            if (field.empty()) continue;
            if (!foundScore &&
                field.find_first_not_of("0123456789.") == std::string::npos)
            {
                try
                {
                    question.materiality = std::clamp(std::stod(field), 0.0, 1.0);
                    foundScore = true;
                    continue;
                }
                catch (const std::exception&)
                {
                }
            }
            if (field.size() > bestText.size()) bestText = field;
        }
        if (bestText.size() < 8) continue;
        question.text = bestText;
        questions.push_back(std::move(question));
    }
    return questions;
}

std::string InvestigationAgent::BuildRoundEnvelope(
    const RoundRequest& request,
    const std::string& identityPosture,
    const bool checksAreAvailable)
{
    std::ostringstream envelope;
    envelope << identityPosture << "\n\n";
    envelope << "You are partway through working out this:\n" << request.goal << "\n\n";

    if (!request.priorFindings.empty())
    {
        // The whole point of the loop. What follows is what earlier rounds actually
        // established, so this round's questions can be chosen because of it rather than
        // guessed at again from the goal alone.
        envelope << "What you have already found:\n";
        for (const Finding& finding : request.priorFindings)
        {
            envelope << "- [" << ToString(finding.kind) << "] " << finding.observed;
            if (!finding.limitations.empty())
            {
                envelope << " (limits: " << finding.limitations << ")";
            }
            envelope << "\n";
        }
        envelope << "\n";
    }
    if (!request.hypotheses.empty())
    {
        envelope << "Working ideas:\n";
        for (const Hypothesis& hypothesis : request.hypotheses)
        {
            envelope << "- (" << ToString(hypothesis.standing) << ") " << hypothesis.text
                     << "\n";
        }
        envelope << "\n";
    }

    envelope << "Questions to settle now:\n";
    for (const InvestigationQuestion& question : request.questions)
    {
        envelope << "  " << question.id << ": " << question.text << "\n";
    }
    envelope << "\n";

    if (checksAreAvailable)
    {
        envelope << "You may consult configuration, source, logs, tests, or file state.\n";
    }
    else
    {
        // Said plainly, because a model told nothing about its tools will assume it has
        // them, and an invented "I checked the logs" is the failure that matters most.
        envelope
            << "You have NO tools in this round. You cannot read files, run tests, or "
               "consult logs. Answer only from what you already know and from the "
               "findings above, and mark anything you would need to look up as blocked. "
               "Do not describe checks you did not perform.\n";
    }

    envelope
        << "\nReply with these lines and nothing else:\n"
           "FOUND <question id> | <supported|refuted|unresolved> | <reasoning|config|"
           "source|logs|tests|file> | <what you did> | <what it shows>\n"
           "BLOCKED <question id> | <what you would need and cannot get>\n"
           "LIMIT <question id> | <what this does not establish>\n"
           "NEXT <materiality 0.0-1.0> | <a question these findings raise>\n"
           "IDEA <a hypothesis worth testing>\n"
           "AGAINST <question id> | <the idea this contradicts>\n"
           "DONE <why the original question is now answered>\n\n"
           "Raise a NEXT only when a finding above actually raises it. Use DONE only when "
           "the original question is genuinely settled.";
    return envelope.str();
}

RoundResult InvestigationAgent::ParseRound(
    const std::string& raw,
    const RoundRequest& request,
    const bool checksAreAvailable)
{
    RoundResult result;
    std::istringstream stream(raw);
    std::string line;

    // Collected first so LIMIT and AGAINST can attach to the outcome they qualify,
    // whatever order the model wrote them in.
    std::vector<std::pair<std::string, std::string>> limits;
    std::vector<std::pair<std::string, std::string>> against;

    while (std::getline(stream, line))
    {
        line = Trim(line);
        if (line.empty()) continue;

        if (StartsWith(line, "FOUND"))
        {
            const std::vector<std::string> fields = SplitFields(line.substr(5), '|');
            if (fields.size() < 5) continue;
            const InvestigationQuestion* question = Resolve(request, fields[0]);
            if (question == nullptr) continue;

            CheckOutcome outcome;
            outcome.questionId = question->id;
            outcome.status = ParseStatus(fields[1]);
            const CheckKind claimed = ParseCheckKind(fields[2]);

            // The integrity rule. With no executor wired nothing was consulted, so a
            // claim of having consulted something is recorded as reasoning and the claim
            // itself is kept visible in the description rather than quietly dropped.
            if (!checksAreAvailable && ProducesObservation(claimed))
            {
                outcome.checkKind = CheckKind::ModelReasoning;
                outcome.checkDescription =
                    "reasoning only (no tools were available; it described this as '" +
                    fields[3] + "')";
                outcome.limitations =
                    "Nothing was actually consulted -- this is her reading, not a "
                    "measurement.";
            }
            else
            {
                outcome.checkKind = claimed;
                outcome.checkDescription = fields[3];
            }
            outcome.observed = fields[4];
            if (!outcome.observed.empty()) result.outcomes.push_back(std::move(outcome));
            continue;
        }
        if (StartsWith(line, "BLOCKED"))
        {
            const std::vector<std::string> fields = SplitFields(line.substr(7), '|');
            if (fields.size() < 2) continue;
            const InvestigationQuestion* question = Resolve(request, fields[0]);
            if (question == nullptr) continue;

            CheckOutcome outcome;
            outcome.questionId = question->id;
            outcome.refused = true;
            outcome.refusalReason = fields[1];
            outcome.checkDescription = "could not be checked";
            result.outcomes.push_back(std::move(outcome));
            continue;
        }
        if (StartsWith(line, "LIMIT"))
        {
            const std::vector<std::string> fields = SplitFields(line.substr(5), '|');
            if (fields.size() >= 2) limits.emplace_back(fields[0], fields[1]);
            continue;
        }
        if (StartsWith(line, "AGAINST"))
        {
            const std::vector<std::string> fields = SplitFields(line.substr(7), '|');
            if (fields.size() >= 2) against.emplace_back(fields[0], fields[1]);
            continue;
        }
        if (StartsWith(line, "NEXT"))
        {
            const std::vector<std::string> fields = SplitFields(line.substr(4), '|');
            if (fields.empty()) continue;

            // The field layout is not reliable. A live run produced "NEXT | 0.6 | ..."
            // rather than "NEXT 0.6 | ...", and taking field one as the text recorded a
            // question whose entire content was "0.6" -- which then occupied a slot in
            // the investigation and showed up in the transcript as a question.
            //
            // So the fields are identified by what they are rather than where they sit:
            // the score is whichever field parses as a number, and the question is the
            // longest field that does not.
            ProposedQuestion question;
            question.materiality = 0.5;
            bool foundScore = false;
            std::string bestText;
            for (const std::string& field : fields)
            {
                if (field.empty()) continue;
                try
                {
                    const double value = std::stod(field);
                    // A bare number is a score, not a question, whatever position it is
                    // in. Only the first one is taken.
                    if (!foundScore && value >= 0.0 && value <= 1.0 &&
                        field.find_first_not_of("0123456789.") == std::string::npos)
                    {
                        question.materiality = value;
                        foundScore = true;
                        continue;
                    }
                }
                catch (const std::exception&)
                {
                    // Not a number, so it is a candidate for the question text.
                }
                if (field.size() > bestText.size()) bestText = field;
            }
            // Too short to be a question. Silently dropping it is right: a fragment
            // would occupy a slot and appear in the transcript as though she had asked
            // something.
            if (bestText.size() < 8) continue;
            question.text = bestText;
            result.followUps.push_back(std::move(question));
            continue;
        }
        if (StartsWith(line, "IDEA"))
        {
            const std::string text = Trim(line.substr(4));
            if (!text.empty()) result.newHypotheses.push_back(ProposedHypothesis{text});
            continue;
        }
        if (StartsWith(line, "DONE"))
        {
            result.proposeComplete = true;
            result.completionRationale = Trim(line.substr(4));
            continue;
        }
    }

    for (const auto& limit : limits)
    {
        const InvestigationQuestion* question = Resolve(request, limit.first);
        if (question == nullptr) continue;
        for (CheckOutcome& outcome : result.outcomes)
        {
            if (outcome.questionId != question->id) continue;
            outcome.limitations = outcome.limitations.empty()
                ? limit.second : (outcome.limitations + " " + limit.second);
        }
    }
    for (const auto& contradiction : against)
    {
        const InvestigationQuestion* question = Resolve(request, contradiction.first);
        if (question == nullptr) continue;
        for (CheckOutcome& outcome : result.outcomes)
        {
            if (outcome.questionId != question->id) continue;
            // Matched against the hypothesis text the loop already holds; the loop
            // resolves the id when it records the finding.
            for (const Hypothesis& hypothesis : request.hypotheses)
            {
                if (hypothesis.text.find(contradiction.second) != std::string::npos ||
                    contradiction.second.find(hypothesis.text) != std::string::npos)
                {
                    outcome.contradictsHypotheses.push_back(hypothesis.id);
                }
            }
        }
    }
    return result;
}

RoundRunner InvestigationAgent::MakeRunner(
    const messageRouter& router,
    std::string identityPosture,
    CheckExecutor executor,
    const std::stop_token stopToken)
{
    return [&router, posture = std::move(identityPosture),
            executor = std::move(executor), stopToken](const RoundRequest& request)
    {
        RoundResult result;
        if (stopToken.stop_requested()) return result;

        const bool checksAvailable = static_cast<bool>(executor);
        const responseOutput response = router.Deliberate(
            InvestigationAgent::BuildRoundEnvelope(request, posture, checksAvailable),
            stopToken);
        // One model call per round, counted honestly whether or not it produced anything.
        result.tokensUsed = response.response.size() / 4;
        if (!response.bSuccess) return result;

        result = InvestigationAgent::ParseRound(
            response.response, request, checksAvailable);
        result.tokensUsed = response.response.size() / 4;

        if (!checksAvailable) return result;

        // With an executor wired, every proposed check actually runs through the existing
        // permission and audit paths. What it returns replaces what the model described,
        // because the observation is the executor's to report, not the model's.
        for (CheckOutcome& outcome : result.outcomes)
        {
            if (outcome.refused || !ProducesObservation(outcome.checkKind)) continue;
            const InvestigationQuestion* question = nullptr;
            for (const InvestigationQuestion& candidate : request.questions)
            {
                if (candidate.id == outcome.questionId) question = &candidate;
            }
            const ExecutedCheck executed = executor(
                outcome.checkKind, outcome.checkDescription,
                question != nullptr ? question->text : std::string{});
            ++result.toolCallsUsed;
            if (!executed.ran)
            {
                outcome.refused = true;
                outcome.refusalReason = executed.refusal.empty()
                    ? "the check did not run" : executed.refusal;
                continue;
            }
            outcome.observed = executed.observed;
            if (!executed.limitations.empty())
            {
                outcome.limitations = outcome.limitations.empty()
                    ? executed.limitations
                    : (outcome.limitations + " " + executed.limitations);
            }
        }
        return result;
    };
}

} // namespace revia::agents
