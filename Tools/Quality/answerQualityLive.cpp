#include "Audit/contentDigest.h"
#include "Evaluation/conversationEvaluation.h"
#include "Runtime/conversationRuntime.h"
#include "observedLocalModel.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace
{
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
        if (error)
        {
            std::cerr << "Cannot restore the developer tool's working directory: " << error.message() << '\n';
        }
    }

  private:
    std::filesystem::path previous;
};

void WriteEvidence(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    output.flush();
    output.close();
    if (!output.good())
    {
        throw std::runtime_error("Cannot retain the actual quality report or metadata.");
    }
}
}

int main(int argc, char** argv)
{
    if (argc < 5 || argc > 6 || (argc == 6 && std::string(argv[5]) != "review"))
    {
        std::cerr << "Usage: ReviaAnswerQualityLive <profile.json> <corpus.json> <loopback-port> <report.jsonl> [review]\n";
        return 2;
    }
    try
    {
        using namespace revia::runtime;
        const auto profilePath = std::filesystem::absolute(argv[1]);
        const auto corpusPath = std::filesystem::absolute(argv[2]);
        const auto output = std::filesystem::absolute(argv[4]);
        const auto runtimeDirectory = std::filesystem::path(output.string() + ".runtime");
        std::ifstream profileFile(profilePath);
        const auto authored = nlohmann::json::parse(profileFile);
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
        std::vector<revia::evaluation::EvaluationCase> cases;
        std::string error;
        if (!revia::evaluation::ConversationEvaluator::LoadCorpus(corpusPath, cases, error))
            throw std::runtime_error(error);
        const int upstreamPort = std::stoi(argv[3]);
        if (upstreamPort <= 0 || upstreamPort > 65535)
            throw std::runtime_error("Invalid loopback port.");
        const bool review = argc == 6;
        std::filesystem::create_directories(output.parent_path());
        if (std::filesystem::exists(runtimeDirectory) && !std::filesystem::is_empty(runtimeDirectory))
        {
            throw std::runtime_error("Retained runtime evidence already exists; choose a fresh report path.");
        }
        std::filesystem::create_directories(runtimeDirectory);
        const ScopedWorkingDirectory workingDirectory(runtimeDirectory);
        revia::quality::ObservedLocalModel observed(upstreamPort);
        try
        {
            messageRouter router;
            llmSettings settings;
            settings.host = "127.0.0.1";
            settings.port = observed.port;
            settings.modelName = "Qwen3.5-4B-Q4_K_M.gguf";
            settings.bAutoStartServer = false;
            settings.bShutdownServerOnExit = false;
            settings.bVisionEnabled = false;
            settings.bAutoMaxTokens = false;
            settings.contextSize = 8192;
            settings.maxTokens = 512;
            embeddingSettings embeddings;
            embeddings.bEnabled = false;
            router.ApplyLLMSettings(settings, embeddings, profile);
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
                [review]
                {
                    responseFilterSettings filters;
                    filters.bAiReviewEnabled = review;
                    return filters;
                },
                [] { return std::string{}; });
            auto report = revia::evaluation::ConversationEvaluator::Run(
                cases, [&](const std::string& input, const std::vector<conversationMessage>& prior)
                { return runtime.EvaluateTurn(input, prior, profile, true); }, settings.modelName);
            std::ifstream corpusFile(corpusPath);
            const auto corpus = nlohmann::json::parse(corpusFile);
            const auto& authoredCases = corpus.is_array() ? corpus : corpus.at("cases");
            report.passed = 0;
            report.failed = 0;
            for (std::size_t index = 0; index < report.cases.size(); ++index)
            {
                auto& outcome = report.cases[index];
                for (std::size_t turn = 0; turn < outcome.turns.size(); ++turn)
                {
                    const auto count = authoredCases.at(index).at("turns").at(turn).value("exactSentences", std::size_t{0});
                    if (count > 0 && outcome.turns[turn].modelSucceeded &&
                        revia::evaluation::ConversationEvaluator::CountSentences(outcome.turns[turn].reply) != count)
                        outcome.turns[turn].failures.push_back("Requested exactly " + std::to_string(count) + " sentences.");
                }
                if (!outcome.unavailable)
                {
                    if (outcome.Passed())
                        ++report.passed;
                    else
                        ++report.failed;
                }
            }
            observed.Close();
            observed.SaveTraffic(output);
            WriteEvidence(output, report.ToJsonLines());
            WriteEvidence(output.string() + ".metadata.json",
                nlohmann::json{{"provider", settings.modelName}, {"host", "127.0.0.1"}, {"upstreamPort", upstreamPort},
                    {"observedPort", observed.port}, {"routerPort", settings.port}, {"profileId", profile.id},
                    {"profileDigest", revia::audit::ContentDigest(authored.dump())},
                    {"profileTemperatureOverride", profile.bHasTemperatureOverride}, {"temperature", profile.temperature},
                    {"maxTokens", settings.maxTokens}, {"contextSize", settings.contextSize}, {"optionalReview", review},
                    {"runtimeDirectory", runtimeDirectory.string()}, {"requests", output.string() + ".requests.json"},
                    {"responses", output.string() + ".responses.json"},
                    {"personalityApproval", "Owner review required; phrase checks cannot establish character quality"},
                    {"physicalVoice", "Not exercised; no speech or memory is queued"}}
                        .dump(2) +
                    '\n');
            std::cout << report.Detail();
            return report.unavailable > 0 ? 2 : report.failed > 0 ? 1 : 0;
        }
        catch (...)
        {
            observed.Close();
            observed.SaveTraffic(output);
            throw;
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "Quality comparison unavailable: " << error.what() << '\n';
        return 2;
    }
}
