#include "Audit/contentDigest.h"
#include "Evaluation/answerGate.h"
#include "Runtime/conversationRuntime.h"
#include "observedLocalModel.h"
#include "routerJudge.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

// The answer-quality gate. It runs the golden set through the real conversation path
// against a local model, scores each reply with the deterministic checks and a judge
// model, and compares the overall pass rate with the stored baseline.
//
// Exit codes: 0 passed or no baseline yet, 1 regressed beyond the allowance, 2 the run
// could not be judged (model unreachable, too many unjudged cases, bad arguments).

namespace
{
using Json = nlohmann::json;

constexpr const char* Usage =
    "Usage: ReviaAnswerGateLive <profile.json> <golden-set.jsonl> <loopback-port> [options]\n"
    "       ReviaAnswerGateLive --validate <golden-set.jsonl>\n"
    "Options:\n"
    "  --output-dir <dir>          results directory (default RuntimeData/Evaluation)\n"
    "  --baseline <file>           baseline summary (default Config/Evaluation/golden-baseline.json when present)\n"
    "  --max-drop <points>         allowed pass-rate drop in percentage points (default 3)\n"
    "  --max-unavailable <percent> unjudged cases allowed before the run is incomplete (default 5)\n"
    "  --min-judge-coverage <pct>  judge verdicts required, as a share of answered cases (default 90)\n"
    "  --judge-port <port>         llama.cpp server for the judge's Main tier (default: the loopback port)\n"
    "  --judge-expert-port <port>  llama.cpp server for an Expert judge, preferred when given\n"
    "  --no-judge                  deterministic checks only\n"
    "  --category <name>           run one category; repeat for several\n"
    "  --write-baseline <file>     also write this run as a new baseline file\n"
    "  --commit <id>               source revision recorded with the results\n";

class ScopedWorkingDirectory
{
  public:
    explicit ScopedWorkingDirectory(const std::filesystem::path& path) : previous(std::filesystem::current_path())
    {
        std::filesystem::current_path(path);
    }

    ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
    ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;

    ~ScopedWorkingDirectory()
    {
        std::error_code error;
        std::filesystem::current_path(previous, error);
    }

  private:
    std::filesystem::path previous;
};

struct Options
{
    std::filesystem::path profile;
    std::filesystem::path goldenSet;
    int port = 0;
    std::filesystem::path outputDirectory = "RuntimeData/Evaluation";
    std::filesystem::path baseline;
    bool baselineExplicit = false;
    std::filesystem::path writeBaseline;
    int judgePort = 0;
    int judgeExpertPort = 0;
    std::set<std::string> categories;
    std::string commit;
    revia::evaluation::GateOptions gate;
};

int ParsePort(const std::string& text)
{
    const int port = std::stoi(text);
    if (port <= 0 || port > 65535)
        throw std::runtime_error("Invalid loopback port: " + text);
    return port;
}

double ParsePercent(const std::string& text)
{
    const double value = std::stod(text);
    if (value < 0.0 || value > 100.0)
        throw std::runtime_error("A percentage must be between 0 and 100: " + text);
    return value;
}

Options ParseOptions(const int argc, char** argv)
{
    if (argc < 4)
        throw std::invalid_argument("Missing arguments.");
    Options options;
    options.profile = std::filesystem::absolute(argv[1]);
    options.goldenSet = std::filesystem::absolute(argv[2]);
    options.port = ParsePort(argv[3]);
    options.baseline = "Config/Evaluation/golden-baseline.json";
    for (int index = 4; index < argc; ++index)
    {
        const std::string flag = argv[index];
        const auto value = [&]() -> std::string
        {
            if (index + 1 >= argc)
                throw std::invalid_argument(flag + " needs a value.");
            return argv[++index];
        };
        if (flag == "--output-dir")
            options.outputDirectory = value();
        else if (flag == "--baseline")
        {
            options.baseline = value();
            options.baselineExplicit = true;
        }
        else if (flag == "--max-drop")
            options.gate.maximumDropPoints = ParsePercent(value());
        else if (flag == "--max-unavailable")
            options.gate.maximumUnavailablePercent = ParsePercent(value());
        else if (flag == "--min-judge-coverage")
            options.gate.minimumJudgeCoveragePercent = ParsePercent(value());
        else if (flag == "--judge-port")
            options.judgePort = ParsePort(value());
        else if (flag == "--judge-expert-port")
            options.judgeExpertPort = ParsePort(value());
        else if (flag == "--no-judge")
            options.gate.judgeEnabled = false;
        else if (flag == "--category")
            options.categories.insert(value());
        else if (flag == "--write-baseline")
            options.writeBaseline = value();
        else if (flag == "--commit")
            options.commit = value();
        else
            throw std::invalid_argument("Unknown option: " + flag);
    }
    options.outputDirectory = std::filesystem::absolute(options.outputDirectory);
    options.baseline = std::filesystem::absolute(options.baseline);
    if (!options.writeBaseline.empty())
        options.writeBaseline = std::filesystem::absolute(options.writeBaseline);
    if (options.judgePort == 0)
        options.judgePort = options.port;
    return options;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read " + path.string() + '.');
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, const std::string& text, const bool append = false)
{
    std::ofstream output(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    output << text;
    output.close();
    if (!output.good())
        throw std::runtime_error("Cannot write " + path.string() + '.');
}

std::string UtcStamp(const char* format)
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, format);
    return stream.str();
}

// The authored profile, read exactly as the existing answer-quality tool reads it.
aiProfile LoadProfile(const Json& authored)
{
    aiProfile profile;
    profile.id = authored.at("id").get<std::string>();
    profile.displayName = authored.at("displayName").get<std::string>();
    profile.systemPrompt = authored.at("systemPrompt").get<std::string>();
    profile.bMemoryEnabled = false;
    profile.bHasTemperatureOverride = authored.contains("temperature");
    profile.temperature = authored.value("temperature", 0.8F);
    profile.personalityBaseline = authored.value("personalityBaseline", std::map<std::string, float>{});
    const std::string mode = authored.value("answerObligation", std::string("balanced"));
    if (mode == "reliable")
        profile.answerObligation = AnswerObligationMode::Reliable;
    else if (mode == "characterFirst" || mode == "character-first")
        profile.answerObligation = AnswerObligationMode::CharacterFirst;
    else if (mode != "balanced")
        throw std::runtime_error("Unknown authored answer obligation.");
    return profile;
}

// The one model the server reports, or the fallback when it reports none or several.
std::string ObserveModelName(const int port, const std::string& fallback)
{
    httplib::Client probe("127.0.0.1", port);
    probe.set_connection_timeout(3, 0);
    probe.set_read_timeout(5, 0);
    if (const auto inventory = probe.Get("/v1/models"); inventory && inventory->status == 200)
    {
        const auto parsed = Json::parse(inventory->body, nullptr, false);
        if (parsed.is_object() && parsed.contains("data") && parsed.at("data").is_array() && parsed.at("data").size() == 1 &&
            parsed.at("data").front().contains("id") && parsed.at("data").front().at("id").is_string())
            return parsed.at("data").front().at("id").get<std::string>();
    }
    return fallback;
}

llmSettings LocalSettings(const int port, const std::string& modelName)
{
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.modelName = modelName;
    settings.bAutoStartServer = false;
    settings.bShutdownServerOnExit = false;
    settings.bVisionEnabled = false;
    settings.bAutoMaxTokens = false;
    settings.contextSize = 8192;
    settings.maxTokens = 512;
    return settings;
}

void PrintSummary(const revia::evaluation::GateSummary& summary, const revia::evaluation::GateDecision& decision)
{
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "\nAnswer-quality gate: " << summary.overall.passed << '/' << (summary.overall.passed + summary.overall.failed)
              << " cases passed (" << summary.overall.PassRate() << "%), " << summary.overall.unavailable << " unjudged";
    if (summary.overall.judged > 0)
        std::cout << ", mean judge score " << std::setprecision(2) << summary.overall.MeanJudgeScore() << std::setprecision(1);
    std::cout << '\n';
    for (const auto& [name, category] : summary.categories)
        std::cout << "  " << std::left << std::setw(12) << name << std::right << std::setw(4) << category.passed << '/'
                  << (category.passed + category.failed) << "  " << std::setw(5) << category.PassRate() << "%  judge "
                  << std::setprecision(2) << category.MeanJudgeScore() << std::setprecision(1) << '\n';
    if (summary.preferredCandidate + summary.preferredBaseline + summary.ties > 0)
        std::cout << "  pairwise vs baseline: candidate " << summary.preferredCandidate << ", baseline " << summary.preferredBaseline
                  << ", tie " << summary.ties << '\n';
    std::cout << "Gate: " << revia::evaluation::ToString(decision.outcome);
    if (decision.comparedCases > 0)
        std::cout << " (" << decision.comparedCases << " cases compared, baseline " << decision.baselinePassRate << "%, now "
                  << decision.currentPassRate << "%, drop " << decision.dropPoints << " points)";
    std::cout << '\n';
    for (const auto& note : decision.notes)
        std::cout << "  - " << note << '\n';
}

int Validate(const std::filesystem::path& path)
{
    std::vector<revia::evaluation::GoldenCase> cases;
    std::string error;
    if (!revia::evaluation::LoadGoldenSet(path, cases, error))
    {
        std::cerr << "Golden set refused: " << error << '\n';
        return 2;
    }
    std::map<std::string, std::size_t> counts;
    for (const auto& item : cases)
        ++counts[item.category];
    std::cout << cases.size() << " golden cases";
    for (const auto& [name, count] : counts)
        std::cout << ", " << name << ' ' << count;
    std::cout << '\n';
    return 0;
}
}

int main(int argc, char** argv)
{
    using namespace revia::evaluation;
    if (argc == 3 && std::string(argv[1]) == "--validate")
        return Validate(argv[2]);
    Options options;
    try
    {
        options = ParseOptions(argc, argv);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n' << Usage;
        return 2;
    }
    try
    {
        using namespace revia::runtime;
        const auto authored = Json::parse(ReadFile(options.profile));
        const aiProfile profile = LoadProfile(authored);

        const std::string corpusBytes = ReadFile(options.goldenSet);
        std::vector<GoldenCase> cases;
        std::string error;
        if (!ParseGoldenSet(corpusBytes, cases, error))
            throw std::runtime_error("Golden set refused: " + error);
        if (!options.categories.empty())
        {
            std::erase_if(cases, [&](const GoldenCase& item) { return !options.categories.contains(item.category); });
            if (cases.empty())
                throw std::runtime_error("No golden case is in the selected categories.");
        }

        std::optional<GateBaseline> baseline;
        if (std::filesystem::exists(options.baseline))
        {
            GateBaseline loaded;
            if (!ParseGateBaseline(ReadFile(options.baseline), loaded, error))
                throw std::runtime_error("Baseline refused: " + error);
            baseline = std::move(loaded);
        }
        else if (options.baselineExplicit)
            throw std::runtime_error("No baseline at " + options.baseline.string() + '.');

        const std::string stamp = UtcStamp("%Y-%m-%dT%H-%M-%SZ");
        std::filesystem::create_directories(options.outputDirectory);
        const auto report = options.outputDirectory / (stamp + ".jsonl");
        const auto summaryPath = options.outputDirectory / (stamp + ".summary.json");
        const auto historyPath = options.outputDirectory / "history.jsonl";
        const auto runtimeDirectory = options.outputDirectory / (stamp + ".runtime");
        if (std::filesystem::exists(report) || (std::filesystem::exists(runtimeDirectory) && !std::filesystem::is_empty(runtimeDirectory)))
            throw std::runtime_error("Results for " + stamp + " already exist; run again in a second.");
        std::filesystem::create_directories(runtimeDirectory);
        WriteFile(runtimeDirectory / "golden-set.jsonl", corpusBytes);

        // Disposable runtime state goes beside the results, never into the owner's data.
        const ScopedWorkingDirectory workingDirectory(runtimeDirectory);
        revia::quality::ObservedLocalModel observed(options.port);
        std::unique_ptr<revia::quality::ObservedLocalModel> judgeObserved;
        std::unique_ptr<revia::quality::ObservedLocalModel> expertObserved;
        bool closed = false;
        const auto closeAll = [&]
        {
            if (std::exchange(closed, true))
                return;
            observed.Close();
            observed.SaveTraffic(report);
            if (judgeObserved)
            {
                judgeObserved->Close();
                judgeObserved->SaveTraffic(options.outputDirectory / (stamp + ".judge"));
            }
            if (expertObserved)
            {
                expertObserved->Close();
                expertObserved->SaveTraffic(options.outputDirectory / (stamp + ".judge-expert"));
            }
        };
        try
        {
            const std::string modelName = ObserveModelName(options.port, "local-model");
            messageRouter router;
            embeddingSettings embeddings;
            embeddings.bEnabled = false;
            router.ApplyLLMSettings(LocalSettings(observed.port, modelName), embeddings, profile);
            conversationContext history;
            revia::agents::TurnCoordinator coordinator;
            revia::speech::SpeechService speech;
            AffectController affect;
            revia::emotion::EmotionRuntime emotions;
            RuntimeEventBus events;
            logger log;
            ConversationRuntime runtime(
                router, history, coordinator, speech, affect, emotions, events, log, [](RuntimeState, const std::string&) {},
                [](const AffectSnapshot&) {}, [] { return revia::actions::CapabilitySettings::InternetAccess{}; },
                [] { return revia::actions::CapabilitySettings::DesktopControl{}; },
                [](const std::string&, const std::string&) { return revia::actions::ActionOutcome{}; },
                [] { return responseFilterSettings{}; }, [] { return std::string{}; });

            std::vector<EvaluationCase> converted;
            for (const auto& item : cases)
                converted.push_back(ToEvaluationCase(item));
            std::cout << "Running " << cases.size() << " golden cases against " << modelName << "...\n" << std::flush;
            const auto started = std::chrono::steady_clock::now();
            const auto evaluated = ConversationEvaluator::Run(converted, [&](const std::string& input, const std::vector<conversationMessage>& prior)
                { return runtime.EvaluateTurn(input, prior, profile, true); }, modelName);
            auto results = ScoreGoldenReplies(cases, evaluated);

            // The judge has its own router, so its requests never share the candidate's
            // posture, reply note or retained traffic.
            std::string judgeIdentity;
            messageRouter judgeRouter;
            if (options.gate.judgeEnabled)
            {
                judgeObserved = std::make_unique<revia::quality::ObservedLocalModel>(options.judgePort);
                const auto judgeModel = ObserveModelName(options.judgePort, modelName);
                auto judgeMain = LocalSettings(judgeObserved->port, judgeModel);
                llmSettings judgeExpert;
                if (options.judgeExpertPort > 0)
                {
                    expertObserved = std::make_unique<revia::quality::ObservedLocalModel>(options.judgeExpertPort);
                    judgeExpert = LocalSettings(expertObserved->port, ObserveModelName(options.judgeExpertPort, "expert-model"));
                    judgeIdentity = "local:Expert:" + judgeExpert.modelName + "@" + std::to_string(options.judgeExpertPort) +
                                    " (Main fallback " + judgeModel + ")";
                }
                else
                    judgeIdentity = "local:Main:" + judgeModel + "@" + std::to_string(options.judgePort);
                judgeRouter.ApplyLLMSettings(judgeMain, {}, judgeExpert, embeddings, profile, false, options.judgeExpertPort > 0);
                const AnswerJudge judge(std::make_shared<revia::quality::RouterJudgeBackend>(judgeRouter, judgeIdentity));
                std::size_t index = 0;
                for (auto& result : results)
                {
                    ++index;
                    if (!result.available)
                        continue;
                    std::optional<std::string> stored;
                    if (baseline)
                        if (const auto found = baseline->cases.find(result.id); found != baseline->cases.end())
                            stored = found->second.finalReply;
                    const auto item = std::find_if(cases.begin(), cases.end(), [&](const GoldenCase& value) { return value.id == result.id; });
                    result.judge = judge.Judge(BuildJudgeInput(*item, result, stored));
                    std::cout << "  judged " << index << '/' << results.size() << ' ' << result.id << ": "
                              << (result.judge.available ? std::to_string(result.judge.score) : "unavailable") << '\n'
                              << std::flush;
                }
            }
            const double elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

            auto summary = SummarizeGoldenResults(results);
            summary.timestamp = UtcStamp("%Y-%m-%dT%H:%M:%SZ");
            summary.model = modelName;
            summary.judge = options.gate.judgeEnabled ? judgeIdentity : "none";
            summary.corpusDigest = revia::audit::ContentDigest(corpusBytes);
            const auto decision = DecideGate(results, baseline ? &*baseline : nullptr, options.gate);

            const Json provenance = {{"commit", options.commit}, {"model", modelName}, {"judge", summary.judge},
                {"corpus", options.goldenSet.string()}, {"corpusDigest", summary.corpusDigest}, {"cases", cases.size()},
                {"categories", options.categories}, {"profileId", profile.id}, {"profileDigest", revia::audit::ContentDigest(authored.dump())},
                {"upstreamPort", options.port}, {"judgePort", options.judgePort}, {"judgeExpertPort", options.judgeExpertPort},
                {"baseline", baseline ? options.baseline.string() : std::string()}, {"baselineModel", baseline ? baseline->model : std::string()},
                {"baselineCorpusDigest", baseline ? baseline->corpusDigest : std::string()}, {"maxTokens", 512}, {"contextSize", 8192},
                {"temperature", profile.temperature}, {"optionalReview", false}, {"repairedTurns", evaluated.repairedTurns},
                {"elapsedSeconds", elapsedSeconds},
                {"options", {{"maxDropPoints", options.gate.maximumDropPoints}, {"maxUnavailablePercent", options.gate.maximumUnavailablePercent},
                                {"minJudgeCoveragePercent", options.gate.minimumJudgeCoveragePercent}, {"judgeEnabled", options.gate.judgeEnabled}}}};
            std::string lines = Json{{"record", "run"}, {"timestamp", summary.timestamp}, {"provenance", provenance}}.dump() + '\n';
            for (const auto& result : results)
                lines += GoldenCaseResultJson(result).dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
            const Json summaryJson = {{"record", "summary"}, {"summary", GateSummaryJson(summary)}, {"gate", GateDecisionJson(decision)},
                {"provenance", provenance}, {"results", report.string()}};
            lines += summaryJson.dump() + '\n';
            closeAll();
            WriteFile(report, lines);
            WriteFile(summaryPath, summaryJson.dump(2) + '\n');
            WriteFile(historyPath, summaryJson.dump() + '\n', true);
            if (!options.writeBaseline.empty())
            {
                if (decision.outcome == GateOutcome::Incomplete)
                    throw std::runtime_error("An incomplete run cannot become the baseline.");
                std::filesystem::create_directories(options.writeBaseline.parent_path());
                WriteFile(options.writeBaseline, GateBaselineJson(results, summary).dump(1, ' ', false, Json::error_handler_t::replace) + '\n');
                std::cout << "Wrote a new baseline to " << options.writeBaseline.string() << ". Review it and commit it deliberately.\n";
            }
            PrintSummary(summary, decision);
            std::cout << "Results: " << report.string() << "\nSummary: " << summaryPath.string() << '\n';
            return options.writeBaseline.empty() ? decision.ExitCode() : 0;
        }
        catch (...)
        {
            closeAll();
            throw;
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "Answer-quality gate unavailable: " << error.what() << '\n';
        return 2;
    }
}
