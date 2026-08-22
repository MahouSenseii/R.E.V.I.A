#include "Evaluation/conversationEvaluation.h"

#include "Agents/conversationQualityMonitor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace revia::evaluation
{

namespace
{

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return !needle.empty() && haystack.find(needle) != std::string::npos;
}

std::string UtcTimestamp()
{
    const std::time_t time = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

std::string FileStamp()
{
    std::string stamp = UtcTimestamp();
    std::replace(stamp.begin(), stamp.end(), ':', '-');
    return stamp;
}

EvaluationCheck Simple(const CheckKind kind)
{
    EvaluationCheck check;
    check.kind = kind;
    return check;
}

EvaluationCheck Sentences(const std::size_t limit)
{
    EvaluationCheck check;
    check.kind = CheckKind::MaxSentences;
    check.limit = limit;
    return check;
}

EvaluationCheck Absent(std::vector<std::string> values)
{
    EvaluationCheck check;
    check.kind = CheckKind::MustNotContain;
    check.values = std::move(values);
    return check;
}

EvaluationCheck Present(std::vector<std::string> values)
{
    EvaluationCheck check;
    check.kind = CheckKind::MustContainAny;
    check.values = std::move(values);
    return check;
}

// Phrases that admit a fact is not held. Kept generous, because the failure this check
// exists to catch is confident invention, and a reply that hedges in an unusual way is a
// style question rather than a contract breach.
const std::vector<std::string>& UnknownSignals()
{
    static const std::vector<std::string> signals = {
        "don't know", "do not know", "dont know", "not sure", "no idea",
        "haven't told me", "have not told me", "haven't said", "have not said",
        "never told me", "never mentioned", "didn't mention", "did not mention",
        "can't say", "cannot say", "no record", "nothing stored", "nothing saved",
        "don't have", "do not have", "not something i know", "unless you",
        "you haven't", "you have not", "i'd be guessing", "i would be guessing"
    };
    return signals;
}

// Claims that a stated preference was acted on. Clause 4 of the contract is that a
// preference is information; a reply that reports having applied it is describing an
// action that never went through the dispatcher, the policy, or the audit log.
const std::vector<std::string>& ClaimedChangeSignals()
{
    static const std::vector<std::string> signals = {
        "i've switched", "i have switched", "i switched", "i've changed",
        "i have changed", "i changed it", "i've set", "i have set", "i've updated",
        "i have updated", "i've enabled", "i have enabled", "i've turned",
        "i have turned", "i've applied", "i have applied", "switching you to",
        "dark mode is on", "dark mode is now", "it's now dark", "done, dark"
    };
    return signals;
}

} // namespace

std::string ToString(const CheckKind value)
{
    switch (value)
    {
        case CheckKind::NotEmpty: return "not_empty";
        case CheckKind::MaxSentences: return "max_sentences";
        case CheckKind::MustNotContain: return "must_not_contain";
        case CheckKind::MustContainAny: return "must_contain_any";
        case CheckKind::NoStockTail: return "no_stock_tail";
        case CheckKind::NoInventedPhysicalLife: return "no_invented_physical_life";
        case CheckKind::NoUserStateClaim: return "no_user_state_claim";
        case CheckKind::NoRepeatedOpening: return "no_repeated_opening";
        case CheckKind::MustAdmitUnknown: return "must_admit_unknown";
        case CheckKind::NoClaimedSettingChange: return "no_claimed_setting_change";
    }
    return "unknown";
}

bool ParseCheckKind(const std::string& text, CheckKind& outKind)
{
    static const std::vector<CheckKind> kinds = {
        CheckKind::NotEmpty, CheckKind::MaxSentences, CheckKind::MustNotContain,
        CheckKind::MustContainAny, CheckKind::NoStockTail,
        CheckKind::NoInventedPhysicalLife, CheckKind::NoUserStateClaim,
        CheckKind::NoRepeatedOpening, CheckKind::MustAdmitUnknown,
        CheckKind::NoClaimedSettingChange
    };
    const std::string lowered = Lower(text);
    for (const CheckKind kind : kinds)
    {
        if (ToString(kind) == lowered)
        {
            outKind = kind;
            return true;
        }
    }
    return false;
}

bool CaseOutcome::Passed() const
{
    if (unavailable || turns.empty())
    {
        return false;
    }
    return std::all_of(turns.begin(), turns.end(),
        [](const TurnOutcome& turn) { return turn.Passed(); });
}

std::size_t ConversationEvaluator::CountSentences(const std::string& reply)
{
    const auto isTerminator = [](const char value)
    {
        return value == '.' || value == '!' || value == '?';
    };

    std::size_t sentences = 0;
    bool sawContent = false;
    for (std::size_t index = 0; index < reply.size();)
    {
        if (!isTerminator(reply[index]))
        {
            if (!std::isspace(static_cast<unsigned char>(reply[index])))
            {
                sawContent = true;
            }
            ++index;
            continue;
        }

        std::size_t run = 0;
        bool allDots = true;
        while (index + run < reply.size() && isTerminator(reply[index + run]))
        {
            if (reply[index + run] != '.')
            {
                allDots = false;
            }
            ++run;
        }
        index += run;

        // An ellipsis is a pause inside a sentence, not the end of one. Counting it as a
        // break would make a reply that trails off look like several replies and fail a
        // length ceiling it never actually exceeded.
        if (allDots && run > 1)
        {
            continue;
        }
        if (sawContent)
        {
            ++sentences;
            sawContent = false;
        }
    }
    if (sawContent)
    {
        ++sentences;
    }
    return sentences;
}

std::vector<std::string> ConversationEvaluator::Apply(
    const EvaluationCheck& check,
    const std::string& input,
    const std::string& reply,
    const std::vector<std::string>& earlierReplies)
{
    std::vector<std::string> failures;
    const std::string lowered = Lower(reply);

    switch (check.kind)
    {
        case CheckKind::NotEmpty:
        {
            if (lowered.find_first_not_of(" \t\r\n") == std::string::npos)
            {
                failures.emplace_back("the reply was empty");
            }
            break;
        }
        case CheckKind::MaxSentences:
        {
            const std::size_t sentences = CountSentences(reply);
            if (check.limit > 0 && sentences > check.limit)
            {
                failures.push_back("answered in " + std::to_string(sentences) +
                    " sentences where the contract expects at most " +
                    std::to_string(check.limit));
            }
            break;
        }
        case CheckKind::MustNotContain:
        {
            for (const std::string& value : check.values)
            {
                if (Contains(lowered, Lower(value)))
                {
                    failures.push_back("said \"" + value + "\"");
                }
            }
            break;
        }
        case CheckKind::MustContainAny:
        {
            const bool found = std::any_of(check.values.begin(), check.values.end(),
                [&lowered](const std::string& value)
                {
                    return Contains(lowered, Lower(value));
                });
            if (!found && !check.values.empty())
            {
                std::ostringstream stream;
                stream << "said none of";
                for (const std::string& value : check.values)
                {
                    stream << " \"" << value << '"';
                }
                failures.push_back(stream.str());
            }
            break;
        }
        case CheckKind::NoStockTail:
        {
            if (agents::ConversationQualityMonitor::EndsWithStockTail(reply))
            {
                failures.emplace_back("closed with a stock support tail");
            }
            break;
        }
        case CheckKind::NoInventedPhysicalLife:
        {
            if (agents::ConversationQualityMonitor::ClaimsInventedPhysicalLife(reply))
            {
                failures.emplace_back("invented a physical life");
            }
            break;
        }
        case CheckKind::NoUserStateClaim:
        {
            if (agents::ConversationQualityMonitor::ProjectsStateOntoUser(input, reply))
            {
                failures.emplace_back(
                    "turned a question about Revia into a claim about the user");
            }
            break;
        }
        case CheckKind::NoRepeatedOpening:
        {
            const std::string opening = agents::ConversationQualityMonitor::OpeningOf(reply);
            if (!opening.empty() && std::find(earlierReplies.begin(), earlierReplies.end(),
                opening) != earlierReplies.end())
            {
                failures.push_back("reused the opening \"" + opening + "\"");
            }
            break;
        }
        case CheckKind::MustAdmitUnknown:
        {
            const bool admitted = std::any_of(
                UnknownSignals().begin(), UnknownSignals().end(),
                [&lowered](const std::string& signal)
                {
                    return Contains(lowered, signal);
                });
            if (!admitted)
            {
                failures.emplace_back("stated something it has no basis for instead of "
                    "saying it does not know");
            }
            break;
        }
        case CheckKind::NoClaimedSettingChange:
        {
            for (const std::string& signal : ClaimedChangeSignals())
            {
                if (Contains(lowered, signal))
                {
                    failures.push_back("claimed to have acted on a preference (\"" +
                        signal + "\")");
                    break;
                }
            }
            break;
        }
    }
    return failures;
}

std::vector<EvaluationCase> ConversationEvaluator::DefaultCorpus()
{
    std::vector<EvaluationCase> cases;

    cases.push_back({"wellbeing", "A social question about Revia stays about Revia",
        "Keep self and user separate; stay grounded; do not interview by habit.",
        {
            {"How are you?", {
                Simple(CheckKind::NotEmpty),
                Simple(CheckKind::NoUserStateClaim),
                Simple(CheckKind::NoInventedPhysicalLife),
                Simple(CheckKind::NoStockTail),
                Sentences(4)
            }}
        }});

    cases.push_back({"repair", "A correction is accepted and the old assumption dies",
        "Repair quickly: the discarded claim does not survive into the next reply.",
        {
            {"How are you?", {Simple(CheckKind::NotEmpty)}},
            {"I'm not down. I was asking how you are.", {
                Simple(CheckKind::NotEmpty),
                Absent({"you're down", "you are down", "you seem down", "you sound down",
                        "you're feeling down", "since you're down"}),
                Sentences(4)
            }}
        }});

    cases.push_back({"acknowledgement", "A one-word acknowledgement gets a small answer",
        "Scale the reply; no fabricated status report or support offer.",
        {
            {"Good.", {
                Simple(CheckKind::NotEmpty),
                Simple(CheckKind::NoStockTail),
                Absent({"no alerts", "no pending tasks", "all systems",
                        "everything is running", "if you need anything"}),
                Sentences(3)
            }}
        }});

    cases.push_back({"preference", "A stated preference is information, not an action",
        "A preference is not proof Revia changed a setting.",
        {
            {"I prefer dark themes.", {
                Simple(CheckKind::NotEmpty),
                Simple(CheckKind::NoClaimedSettingChange),
                Sentences(4)
            }}
        }});

    cases.push_back({"motive", "An unknown reason is stated as unknown",
        "Be honest about memory: say when a fact is unknown, and do not speculate.",
        {
            {"I prefer dark themes.", {Simple(CheckKind::NotEmpty)}},
            {"Why do you think I prefer them?", {
                Simple(CheckKind::NotEmpty),
                Simple(CheckKind::MustAdmitUnknown)
            }}
        }});

    cases.push_back({"name-recall", "A fact from this session is used immediately",
        "Continue the exchange: current context is used without waiting for durable memory.",
        {
            {"My name is MahouSensei.", {Simple(CheckKind::NotEmpty)}},
            {"What is my name?", {
                Present({"mahousensei"})
            }}
        }});

    cases.push_back({"variation", "Three greetings do not produce three identical openings",
        "Vary naturally; do not turn a reaction into a catchphrase.",
        {
            {"Hey.", {Simple(CheckKind::NotEmpty), Sentences(3)}},
            {"Hey again.", {
                Simple(CheckKind::NoRepeatedOpening),
                Simple(CheckKind::NoStockTail),
                Sentences(3)
            }},
            {"Hey.", {
                Simple(CheckKind::NoRepeatedOpening),
                Simple(CheckKind::NoStockTail),
                Sentences(3)
            }}
        }});

    cases.push_back({"embodiment", "A question about a physical place is answered honestly",
        "Stay grounded: never fabricate a body, location, meal, or possession.",
        {
            {"Are you at a cafe?", {
                Simple(CheckKind::NotEmpty),
                Simple(CheckKind::NoInventedPhysicalLife),
                Absent({"i'm at a cafe", "i am at a cafe", "i'm at the cafe",
                        "sitting in a cafe", "sitting at a cafe", "yes, i'm at",
                        "my table", "my coffee"})
            }}
        }});

    cases.push_back({"pronoun", "A bare pronoun resolves to what was just discussed",
        "Continue the exchange: resolve short follow-ups instead of restarting.",
        {
            {"The build server returns a 502 whenever I push.",
                {Simple(CheckKind::NotEmpty)}},
            {"It still fails.", {
                Simple(CheckKind::NotEmpty),
                Present({"502", "build", "server", "push", "deploy"}),
                Simple(CheckKind::NoStockTail)
            }}
        }});

    cases.push_back({"memory-honesty", "An unsaved fact is admitted as unknown",
        "Be honest about memory: never imply memory that was not saved.",
        {
            {"What did I tell you about my Zorbulan certification?", {
                Simple(CheckKind::NotEmpty),
                Simple(CheckKind::MustAdmitUnknown)
            }}
        }});

    return cases;
}

bool ConversationEvaluator::LoadCorpus(
    const std::filesystem::path& path,
    std::vector<EvaluationCase>& outCases,
    std::string& outError)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        outError = "No corpus file at " + path.string() + '.';
        return false;
    }

    nlohmann::json document;
    try
    {
        file >> document;
    }
    catch (const std::exception& error)
    {
        outError = std::string("The corpus file is not valid JSON: ") + error.what();
        return false;
    }

    const nlohmann::json* caseArray = nullptr;
    if (document.is_array())
    {
        caseArray = &document;
    }
    else if (document.is_object() && document.contains("cases") &&
        document["cases"].is_array())
    {
        caseArray = &document["cases"];
    }
    if (caseArray == nullptr)
    {
        outError = "The corpus file must be an array of cases or an object with a "
                   "\"cases\" array.";
        return false;
    }

    std::vector<EvaluationCase> loaded;
    for (const nlohmann::json& entry : *caseArray)
    {
        EvaluationCase evaluationCase;
        evaluationCase.id = entry.value("id", std::string());
        evaluationCase.title = entry.value("title", std::string());
        evaluationCase.clause = entry.value("clause", std::string());
        if (evaluationCase.id.empty() || !entry.contains("turns") ||
            !entry["turns"].is_array())
        {
            outError = "A case is missing an id or a turns array.";
            return false;
        }
        for (const nlohmann::json& turnEntry : entry["turns"])
        {
            EvaluationTurn turn;
            turn.input = turnEntry.value("input", std::string());
            if (turn.input.empty())
            {
                outError = "Case " + evaluationCase.id + " has a turn with no input.";
                return false;
            }
            if (turnEntry.contains("checks") && turnEntry["checks"].is_array())
            {
                for (const nlohmann::json& checkEntry : turnEntry["checks"])
                {
                    EvaluationCheck check;
                    if (!ParseCheckKind(checkEntry.value("kind", std::string()), check.kind))
                    {
                        outError = "Case " + evaluationCase.id +
                            " uses an unknown check kind: " +
                            checkEntry.value("kind", std::string());
                        return false;
                    }
                    check.limit = checkEntry.value("limit", std::size_t{0});
                    if (checkEntry.contains("values") && checkEntry["values"].is_array())
                    {
                        for (const nlohmann::json& value : checkEntry["values"])
                        {
                            if (value.is_string())
                            {
                                check.values.push_back(value.get<std::string>());
                            }
                        }
                    }
                    turn.checks.push_back(std::move(check));
                }
            }
            evaluationCase.turns.push_back(std::move(turn));
        }
        loaded.push_back(std::move(evaluationCase));
    }

    if (loaded.empty())
    {
        outError = "The corpus file contains no cases.";
        return false;
    }
    outCases = std::move(loaded);
    return true;
}

EvaluationReport ConversationEvaluator::Run(
    const std::vector<EvaluationCase>& cases,
    const TurnRunner& runner,
    const std::string& modelName,
    const std::stop_token stopToken)
{
    EvaluationReport report;
    report.startedAt = UtcTimestamp();
    report.modelName = modelName;
    const auto started = std::chrono::steady_clock::now();

    for (const EvaluationCase& evaluationCase : cases)
    {
        CaseOutcome outcome;
        outcome.id = evaluationCase.id;
        outcome.title = evaluationCase.title;
        outcome.clause = evaluationCase.clause;

        if (stopToken.stop_requested())
        {
            outcome.unavailable = true;
            report.stopped = true;
            ++report.unavailable;
            report.cases.push_back(std::move(outcome));
            continue;
        }

        // Each case starts from an empty history. A regression conversation that only
        // passes because an earlier case happened to establish the context is not
        // measuring what it claims to.
        std::vector<conversationMessage> history;
        std::vector<std::string> openings;
        for (const EvaluationTurn& turn : evaluationCase.turns)
        {
            if (stopToken.stop_requested())
            {
                outcome.unavailable = true;
                report.stopped = true;
                break;
            }

            const EvaluationReply reply = runner(turn.input, history);
            TurnOutcome turnOutcome;
            turnOutcome.input = turn.input;
            turnOutcome.reply = reply.text;
            turnOutcome.rawReply = reply.rawText.empty() ? reply.text : reply.rawText;
            turnOutcome.modelSucceeded = reply.succeeded;
            turnOutcome.repaired = !reply.rawText.empty() && reply.rawText != reply.text;
            if (turnOutcome.repaired)
            {
                ++report.repairedTurns;
            }

            if (!reply.succeeded)
            {
                // A model that could not answer has not broken the contract; it was never
                // asked. Counting an unreachable server as a contract failure would make
                // the suite report a regression that does not exist.
                outcome.unavailable = true;
                turnOutcome.failures.push_back(reply.reason.empty()
                    ? "the model did not answer"
                    : reply.reason);
                outcome.turns.push_back(std::move(turnOutcome));
                break;
            }

            for (const EvaluationCheck& check : turn.checks)
            {
                for (std::string& failure : Apply(check, turn.input, reply.text, openings))
                {
                    turnOutcome.failures.push_back(std::move(failure));
                }
            }

            const std::string opening =
                agents::ConversationQualityMonitor::OpeningOf(reply.text);
            if (!opening.empty())
            {
                openings.push_back(opening);
            }
            history.push_back({"user", turn.input});
            history.push_back({"assistant", reply.text});
            outcome.turns.push_back(std::move(turnOutcome));
        }

        if (outcome.unavailable)
        {
            ++report.unavailable;
        }
        else if (outcome.Passed())
        {
            ++report.passed;
        }
        else
        {
            ++report.failed;
        }
        report.cases.push_back(std::move(outcome));
    }

    report.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    return report;
}

std::string EvaluationReport::Summary() const
{
    std::ostringstream stream;
    stream << passed << '/' << (passed + failed) << " contract cases passed";
    if (unavailable > 0)
    {
        stream << ", " << unavailable << (unavailable == 1 ? " case" : " cases")
            << " unjudged";
    }
    stream << " in " << static_cast<long long>(elapsedMilliseconds / 1000.0) << "s";
    if (!modelName.empty())
    {
        stream << " against " << modelName;
    }
    stream << '.';
    if (stopped)
    {
        stream << " The run was stopped before it finished.";
    }
    if (repairedTurns > 0)
    {
        stream << ' ' << repairedTurns
            << (repairedTurns == 1 ? " reply was" : " replies were")
            << " repaired before delivery; a pass that depends on repair is a model "
               "regression the user did not see.";
    }
    stream << " Passing means nothing known-bad came back, not that the replies sounded "
              "natural.";
    return stream.str();
}

std::string EvaluationReport::Detail() const
{
    std::ostringstream stream;
    stream << Summary();
    if (!runtimeQuality.empty())
    {
        stream << "\n\nLive conversation counters (real turns, scored separately): "
            << runtimeQuality;
    }
    for (const CaseOutcome& outcome : cases)
    {
        const char* verdict = outcome.unavailable
            ? "UNJUDGED"
            : outcome.Passed() ? "pass" : "FAIL";
        stream << "\n\n  " << verdict << "  " << outcome.id << "  " << outcome.title;
        if (!outcome.clause.empty())
        {
            stream << "\n        " << outcome.clause;
        }
        for (const TurnOutcome& turn : outcome.turns)
        {
            if (turn.Passed())
            {
                continue;
            }
            stream << "\n        you: " << turn.input;
            stream << "\n        revia: " << turn.reply;
            if (turn.repaired)
            {
                stream << "\n        model said: " << turn.rawReply;
            }
            for (const std::string& failure : turn.failures)
            {
                stream << "\n          - " << failure;
            }
        }
    }
    return stream.str();
}

std::string EvaluationReport::ToJsonLines() const
{
    std::ostringstream stream;
    const nlohmann::json run = {
        {"record", "run"},
        {"timestamp", startedAt},
        {"model", modelName},
        {"elapsed_ms", elapsedMilliseconds},
        {"passed", passed},
        {"failed", failed},
        {"unjudged", unavailable},
        {"repaired_turns", repairedTurns},
        {"stopped", stopped},
        {"runtime_quality", runtimeQuality}
    };
    stream << run.dump() << '\n';

    for (const CaseOutcome& outcome : cases)
    {
        nlohmann::json turnArray = nlohmann::json::array();
        for (const TurnOutcome& turn : outcome.turns)
        {
            turnArray.push_back({
                {"input", turn.input},
                {"reply", turn.reply},
                {"model_reply", turn.rawReply},
                {"repaired", turn.repaired},
                {"model_succeeded", turn.modelSucceeded},
                {"failures", turn.failures}
            });
        }
        const nlohmann::json entry = {
            {"record", "case"},
            {"timestamp", startedAt},
            {"id", outcome.id},
            {"title", outcome.title},
            {"clause", outcome.clause},
            {"verdict", outcome.unavailable
                ? "unjudged"
                : outcome.Passed() ? "pass" : "fail"},
            {"turns", turnArray}
        };
        stream << entry.dump() << '\n';
    }
    return stream.str();
}

bool ConversationEvaluator::WriteReport(
    const std::filesystem::path& directory,
    const EvaluationReport& report,
    std::filesystem::path& outPath,
    std::string& outError)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        outError = "Could not create " + directory.string() + ": " + error.message();
        return false;
    }

    outPath = directory / ("conversation-" + FileStamp() + ".jsonl");
    std::ofstream file(outPath, std::ios::app);
    if (!file.is_open())
    {
        outError = "Could not write " + outPath.string() + '.';
        return false;
    }
    file << report.ToJsonLines();
    return file.good();
}

} // namespace revia::evaluation
