#include "Audit/contentDigest.h"
#include "Evaluation/answerGate.h"
#include "Runtime/conversationRuntime.h"
#include "observedLocalModel.h"
#include "answerGateExperiment.h"
#include "routerJudge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
    "  --ignore-baseline           independent scoring without an existing pairwise baseline\n"
    "  --max-drop <points>         allowed pass-rate drop in percentage points (default 3)\n"
    "  --max-unavailable <percent> unjudged cases allowed before the run is incomplete (default 5)\n"
    "  --min-judge-coverage <pct>  judge verdicts required, as a share of answered cases (default 90)\n"
    "  --judge-port <port>         llama.cpp server for the judge's Main tier (default: the loopback port)\n"
    "  --judge-expert-port <port>  llama.cpp server for an Expert judge, preferred when given\n"
    "  --no-judge                  deterministic checks only\n"
    "  --category <name>           run one category; repeat for several\n"
    "  --write-baseline <file>     also write this run as a new baseline file\n"
    "  --commit <id>               source revision recorded with the results\n"
    "  --api-key-file <file>       explicitly selected candidate credential file\n"
    "  --api-key-env <name>        explicitly selected candidate credential environment variable\n"
    "  --judge-api-key-file/env    same source options for a separate judge endpoint\n"
    "  --judge-expert-api-key-file/env  same source options for an Expert judge endpoint\n"
    "  --expected-model <id>       refuse a candidate endpoint reporting a different model\n"
    "  --seed <integer>            seed candidate and judge completions; recorded in provenance\n"
    "  --context-size <tokens>     declared server context budget (default 8192; does not reconfigure server)\n"
    "  --max-tokens <tokens>       candidate/judge output budget (default 512)\n"
    "  --omit-system-prompt        experiment: omit only the authored profile prompt\n"
    "  --omit-state-packet         experiment: omit only the rendered state packet\n"
    "  --omit-style-guidance       experiment: omit only turn style guidance\n";

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
    bool ignoreBaseline = false;
    std::filesystem::path writeBaseline;
    int judgePort = 0;
    int judgeExpertPort = 0;
    std::set<std::string> categories;
    std::string commit;
    revia::quality::LocalModelCredentialSource candidateCredential;
    revia::quality::LocalModelCredentialSource judgeCredential;
    revia::quality::LocalModelCredentialSource expertCredential;
    revia::quality::AnswerGateExperiment experiment;
    std::optional<std::uint64_t> seed;
    std::string expectedModel;
    int contextSize = 8192;
    int maxTokens = 512;
    revia::evaluation::GateOptions gate;
};

int ParsePort(const std::string& text)
{
    std::size_t consumed = 0;
    const int port = std::stoi(text, &consumed);
    if (consumed != text.size() || port <= 0 || port > 65535)
        throw std::runtime_error("Invalid loopback port: " + text);
    return port;
}

double ParsePercent(const std::string& text)
{
    std::size_t consumed = 0;
    const double value = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value) || value < 0.0 || value > 100.0)
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
        else if (flag == "--ignore-baseline")
            options.ignoreBaseline = true;
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
        else if (flag == "--api-key-file")
            options.candidateCredential.file = std::filesystem::absolute(value());
        else if (flag == "--api-key-env")
            options.candidateCredential.environment = value();
        else if (flag == "--judge-api-key-file")
            options.judgeCredential.file = std::filesystem::absolute(value());
        else if (flag == "--judge-api-key-env")
            options.judgeCredential.environment = value();
        else if (flag == "--judge-expert-api-key-file")
            options.expertCredential.file = std::filesystem::absolute(value());
        else if (flag == "--judge-expert-api-key-env")
            options.expertCredential.environment = value();
        else if (flag == "--expected-model")
            options.expectedModel = value();
        else if (flag == "--context-size" || flag == "--max-tokens")
        {
            const auto text = value();
            std::size_t consumed = 0;
            const int count = std::stoi(text, &consumed);
            if (consumed != text.size() || count <= 0 || count > 1048576)
                throw std::invalid_argument("Token budgets must be between 1 and 1048576.");
            (flag == "--context-size" ? options.contextSize : options.maxTokens) = count;
        }
        else if (flag == "--seed")
        {
            const auto text = value();
            if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("The completion seed must be an unsigned integer.");
            options.seed = std::stoull(text);
        }
        else if (flag == "--omit-system-prompt")
            options.experiment.authoredSystemPrompt = false;
        else if (flag == "--omit-state-packet")
            options.experiment.statePacket = false;
        else if (flag == "--omit-style-guidance")
            options.experiment.styleGuidance = false;
        else
            throw std::invalid_argument("Unknown option: " + flag);
    }
    options.outputDirectory = std::filesystem::absolute(options.outputDirectory);
    options.baseline = std::filesystem::absolute(options.baseline);
    if (!options.writeBaseline.empty())
        options.writeBaseline = std::filesystem::absolute(options.writeBaseline);
    if (options.judgePort == 0)
        options.judgePort = options.port;
    if (options.maxTokens >= options.contextSize)
        throw std::invalid_argument("Output tokens must be smaller than the declared context.");
    if (options.ignoreBaseline && options.baselineExplicit)
        throw std::invalid_argument("Choose --baseline or --ignore-baseline, not both.");
    if (options.judgeExpertPort == 0 && !options.expertCredential.Empty())
        throw std::invalid_argument("Expert credentials require an Expert judge endpoint.");
    options.experiment.ValidateBaselineRequest(!options.writeBaseline.empty(), options.gate.judgeEnabled, !options.categories.empty());
#ifdef REVIA_EVAL_BASELINE_ONLY
    if (!options.experiment.IsControl())
        throw std::invalid_argument("This frozen-pipeline adapter supports the baseline control only.");
#endif
    if (!options.writeBaseline.empty() && std::filesystem::exists(options.writeBaseline))
        throw std::invalid_argument("The baseline destination already exists; select a new path for deliberate review.");
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

void WriteNewFile(const std::filesystem::path& path, const std::string& text)
{
    std::string error;
    if (!revia::evaluation::WriteEvaluationArtifactOnce(path, text, error))
        throw std::runtime_error(error);
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

// A failed or ambiguous discovery is not a measured model identity.
std::string ObserveModelName(const int port, const std::string& apiKey)
{
    httplib::Client probe("127.0.0.1", port);
    probe.set_connection_timeout(3, 0);
    probe.set_read_timeout(5, 0);
    if (!apiKey.empty())
        probe.set_default_headers({{"Authorization", "Bearer " + apiKey}});
    if (const auto inventory = probe.Get("/v1/models"); inventory && inventory->status == 200)
    {
        const auto parsed = Json::parse(inventory->body, nullptr, false);
        if (parsed.is_object() && parsed.contains("data") && parsed.at("data").is_array() && parsed.at("data").size() == 1 &&
            parsed.at("data").front().contains("id") && parsed.at("data").front().at("id").is_string())
        {
            const auto name = parsed.at("data").front().at("id").get<std::string>();
            if (name.empty() || (!apiKey.empty() && name.find(apiKey) != std::string::npos))
                throw std::runtime_error("Model discovery returned an invalid identity.");
            return name;
        }
    }
    throw std::runtime_error("Authenticated model discovery failed or did not identify exactly one model at the selected loopback port.");
}

llmSettings LocalSettings(const int port, const std::string& modelName, const std::string& apiKey, const Options& options)
{
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.modelName = modelName;
    settings.apiKey = apiKey;
    settings.bAutoStartServer = false;
    settings.bShutdownServerOnExit = false;
    settings.bVisionEnabled = false;
    settings.bAutoMaxTokens = false;
    settings.contextSize = options.contextSize;
    settings.maxTokens = options.maxTokens;
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
        const aiProfile authoredProfile = LoadProfile(authored);
        aiProfile profile = authoredProfile;
        if (!options.experiment.authoredSystemPrompt)
            profile.systemPrompt.clear();
        const auto candidateKey = options.candidateCredential.Read();
        const bool inheritJudgeCredential = options.judgeCredential.Empty() && options.judgePort == options.port;
        const auto judgeKey = options.gate.judgeEnabled
                                  ? (inheritJudgeCredential ? candidateKey : options.judgeCredential.Read()) : std::string{};
        const auto expertKey = options.gate.judgeEnabled && options.judgeExpertPort > 0
                                   ? options.expertCredential.Read() : std::string{};

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
        if (!options.ignoreBaseline && std::filesystem::exists(options.baseline))
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
        if (!std::filesystem::create_directory(runtimeDirectory))
            throw std::runtime_error("The isolated evaluation runtime directory is already claimed.");
        WriteFile(runtimeDirectory / "golden-set.jsonl", corpusBytes);

        // Disposable runtime state goes beside the results, never into the owner's data.
        const ScopedWorkingDirectory workingDirectory(runtimeDirectory);
        revia::quality::ObservedLocalModel observed(options.port, candidateKey);
        observed.SetCompletionSeed(options.seed);
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
            const std::string modelName = ObserveModelName(options.port, candidateKey);
            if (!options.expectedModel.empty() && modelName != options.expectedModel)
                throw std::runtime_error("Candidate model identity does not match the explicitly expected model.");
            const auto judgeModel = options.gate.judgeEnabled ? ObserveModelName(options.judgePort, judgeKey) : std::string{};
            const auto expertModel = options.gate.judgeEnabled && options.judgeExpertPort > 0
                                         ? ObserveModelName(options.judgeExpertPort, expertKey) : std::string{};
            messageRouter router;
            embeddingSettings embeddings;
            embeddings.bEnabled = false;
            router.ApplyLLMSettings(LocalSettings(observed.port, modelName, candidateKey, options), embeddings, profile);
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
                {
#ifdef REVIA_EVAL_BASELINE_ONLY
                    return runtime.EvaluateTurn(input, prior, profile, true);
#else
                    return runtime.EvaluateTurnForExperiment(input, prior, profile, true,
                        {options.experiment.statePacket, options.experiment.styleGuidance});
#endif
                }, modelName);
            auto results = ScoreGoldenReplies(cases, evaluated);

            // The judge has its own router, so its requests never share the candidate's
            // posture, reply note or retained traffic.
            std::string judgeIdentity;
            messageRouter judgeRouter;
            if (options.gate.judgeEnabled)
            {
                judgeObserved = std::make_unique<revia::quality::ObservedLocalModel>(options.judgePort, judgeKey);
                judgeObserved->SetCompletionSeed(options.seed);
                auto judgeMain = LocalSettings(judgeObserved->port, judgeModel, judgeKey, options);
                llmSettings judgeExpert;
                if (options.judgeExpertPort > 0)
                {
                    expertObserved = std::make_unique<revia::quality::ObservedLocalModel>(options.judgeExpertPort, expertKey);
                    expertObserved->SetCompletionSeed(options.seed);
                    judgeExpert = LocalSettings(expertObserved->port, expertModel, expertKey, options);
                    judgeIdentity = "local:Expert:" + judgeExpert.modelName + "@" + std::to_string(options.judgeExpertPort) +
                                    " (Main fallback " + judgeModel + ")";
                }
                else
                    judgeIdentity = "local:Main:" + judgeModel + "@" + std::to_string(options.judgePort);
                judgeRouter.ApplyLLMSettings(judgeMain, {}, judgeExpert, embeddings, authoredProfile, false, options.judgeExpertPort > 0);
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
                {"baselineCorpusDigest", baseline ? baseline->corpusDigest : std::string()},
                {"maxTokens", options.maxTokens}, {"contextSize", options.contextSize},
                {"temperature", profile.temperature}, {"optionalReview", false}, {"repairedTurns", evaluated.repairedTurns},
                {"seed", options.seed ? Json(*options.seed) : Json(nullptr)},
                {"promptBlocks", {{"authoredSystemPrompt", options.experiment.authoredSystemPrompt},
                                    {"statePacket", options.experiment.statePacket}, {"styleGuidance", options.experiment.styleGuidance}}},
                {"effectiveSystemPromptDigest", revia::audit::ContentDigest(profile.systemPrompt)},
                {"credentialSources", {{"candidate", options.candidateCredential.Kind()},
                                          {"judge", inheritJudgeCredential ? "candidate" : options.judgeCredential.Kind()},
                                          {"expert", options.expertCredential.Kind()}}},
                {"memoryEnabled", false}, {"actionsEnabled", false}, {"runtimeDirectory", runtimeDirectory.string()},
#ifdef REVIA_EVAL_BASELINE_ONLY
                {"pipelineBuild", "frozen-baseline-adapter"},
#else
                {"pipelineBuild", "current-build"},
#endif
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
            WriteNewFile(report, lines);
            WriteNewFile(summaryPath, summaryJson.dump(2) + '\n');
            WriteFile(historyPath, summaryJson.dump() + '\n', true);
            if (!options.writeBaseline.empty())
            {
                if (decision.outcome == GateOutcome::Incomplete ||
                    std::any_of(results.begin(), results.end(), [](const auto& item) { return !item.available || !item.judge.available; }))
                    throw std::runtime_error("Every corpus case must have an available reply and judge verdict before writing a baseline.");
                std::filesystem::create_directories(options.writeBaseline.parent_path());
                auto baselineJson = GateBaselineJson(results, summary);
                baselineJson["provenance"] = provenance;
                baselineJson["results"] = report.string();
                WriteNewFile(options.writeBaseline, baselineJson.dump(1, ' ', false, Json::error_handler_t::replace) + '\n');
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
