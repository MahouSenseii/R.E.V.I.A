#include "Audit/contentDigest.h"
#include "Evaluation/conversationEvaluation.h"
#include "Runtime/conversationRuntime.h"
#include "cognitionLive.h"
#include "observedLocalModel.h"
#include "privateRuntimeEvaluation.h"
#include "semanticReview.h"

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
    if (argc < 5 || argc > 8 || (argc >= 6 && std::string(argv[5]) != "review" && std::string(argv[5]) != "cognition") ||
        (argc >= 7 && std::string(argv[5]) != "cognition") || (argc == 8 && std::string(argv[7]) != "review"))
    {
        std::cerr << "Usage: ReviaAnswerQualityLive <profile.json> <corpus.json> <loopback-port> <report-path> [review|cognition "
                     "<campaign.json> [review]]\n";
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
        const int upstreamPort = std::stoi(argv[3]);
        if (upstreamPort <= 0 || upstreamPort > 65535)
            throw std::runtime_error("Invalid loopback port.");
        const bool cognition = argc >= 6 && std::string(argv[5]) == "cognition";
        const bool review = (argc == 6 && !cognition) || argc == 8;
        std::optional<revia::evaluation::CampaignManifest> cognitionCampaign;
        if (cognition)
        {
            if (argc < 7)
                throw std::runtime_error("Cognition live collection requires a captured campaign manifest.");
            std::ifstream manifestFile(std::filesystem::absolute(argv[6]), std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(manifestFile)), std::istreambuf_iterator<char>());
            revia::evaluation::CampaignManifest manifest;
            std::string error;
            if (!revia::evaluation::ParseCampaignManifest(bytes, manifest, error) || !manifest.providerAvailable)
                throw std::runtime_error("Captured cognition campaign is invalid or lacks provider evidence: " + error);
            cognitionCampaign = std::move(manifest);
        }
        if (cognition && std::filesystem::exists(output))
        {
            throw std::runtime_error("Retained cognition aggregate already exists; choose a fresh report path.");
        }
        std::filesystem::create_directories(output.parent_path());
        if (std::filesystem::exists(runtimeDirectory) && !std::filesystem::is_empty(runtimeDirectory))
        {
            throw std::runtime_error("Retained runtime evidence already exists; choose a fresh report path.");
        }
        std::filesystem::create_directories(runtimeDirectory);
        std::ifstream corpusFile(corpusPath, std::ios::binary);
        const std::string corpusBytes((std::istreambuf_iterator<char>(corpusFile)), std::istreambuf_iterator<char>());
        const auto corpus = nlohmann::json::parse(corpusBytes);
        const bool privateRuntime = corpus.is_object() && corpus.value("runtimePath", std::string{}) == "private";
        if (privateRuntime && (!cognition || review))
            throw std::runtime_error("Private runtime cohorts require cognition mode without optional AI review.");
        std::optional<revia::quality::PrivateRuntimeEvaluation> privateEvaluation;
        if (privateRuntime)
            privateEvaluation.emplace(corpus);
        if (cognition && cognitionCampaign->fixtureDigest != revia::audit::ContentDigest(corpusBytes))
            throw std::runtime_error("Captured campaign fixtureDigest differs from the exact corpus bytes.");
        const auto capturedCorpusPath = runtimeDirectory / "corpus.json";
        WriteEvidence(capturedCorpusPath, corpusBytes);
        std::vector<revia::evaluation::EvaluationCase> cases;
        std::vector<revia::evaluation::CognitionCase> cognitionCases;
        std::vector<std::uint64_t> cognitionSeeds;
        std::string error;
        if (cognition)
        {
            if (!revia::evaluation::LoadCognitionCorpus(capturedCorpusPath, cognitionCases, cognitionSeeds, error))
                throw std::runtime_error(error);
        }
        else if (!revia::evaluation::ConversationEvaluator::LoadCorpus(capturedCorpusPath, cases, error))
        {
            throw std::runtime_error(error);
        }
        const ScopedWorkingDirectory workingDirectory(runtimeDirectory);
        revia::quality::ObservedLocalModel observed(upstreamPort);
        try
        {
            messageRouter router;
            llmSettings settings;
            settings.host = "127.0.0.1";
            settings.port = observed.port;
            settings.modelName = "Qwen3.5-4B-Q4_K_M.gguf";
            const auto requestedProvider = settings.modelName;
            nlohmann::json providerInventory;
            nlohmann::json providerProperties;
            bool providerObserved = false;
            if (cognition)
            {
                httplib::Client probe("127.0.0.1", upstreamPort);
                probe.set_connection_timeout(3, 0);
                probe.set_read_timeout(3, 0);
                if (const auto inventory = probe.Get("/v1/models"); inventory && inventory->status == 200)
                {
                    providerInventory = nlohmann::json::parse(inventory->body);
                    const auto& data = providerInventory.at("data");
                    if (data.is_array() && data.size() == 1 && data.front().at("id").is_string())
                    {
                        settings.modelName = data.front().at("id").get<std::string>();
                        providerObserved = !settings.modelName.empty();
                    }
                }
                if (const auto properties = probe.Get("/props"); properties && properties->status == 200)
                    providerProperties = nlohmann::json::parse(properties->body);
                revia::quality::RetainCognitionEvidenceOnce(output.string() + ".provider.json",
                    nlohmann::json{{"inventory", providerInventory}, {"properties", providerProperties}}.dump(2) + '\n');
            }
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
            if (cognition)
            {
                const int result = revia::quality::RunCognitionLive(
                    cognitionCases, cognitionSeeds, output, [&](const std::string& input, const std::vector<conversationMessage>& prior)
                    {
                        if (!privateEvaluation)
                            return runtime.EvaluateTurn(input, prior, profile, true);
                        privateEvaluation->Prepare(input, prior, history);
                        const auto firstResponse = observed.Responses().size();
                        const auto answer = privateEvaluation->Reply(router, history, input, profile);
                        const auto responses = observed.Responses();
                        std::string raw;
                        if (responses.size() > firstResponse && responses.back().contains("body"))
                        {
                            const auto body = nlohmann::json::parse(responses.back().at("body").get<std::string>(), nullptr, false);
                            if (!body.is_discarded() && body.contains("choices") && !body.at("choices").empty())
                                raw = body.at("choices").front().at("message").value("content", std::string{});
                        }
                        return revia::evaluation::EvaluationReply{
                            answer.succeeded && answer.fromAssistant && !answer.text.empty(), answer.text, raw, answer.reason,
                            profile.answerObligation};
                    },
                    providerObserved ? settings.modelName : "unverified requested:" + requestedProvider, observed, cognitionCampaign);
                observed.Close();
                observed.SaveTraffic(output);
                if (privateEvaluation)
                    WriteEvidence(output.string() + ".private-runtime.json", privateEvaluation->Observations().dump(2) + '\n');
                WriteEvidence(output.string() + ".metadata.json",
                    nlohmann::json{{"schemaVersion", 1}, {"mode", "cognition"}, {"provider", settings.modelName},
                        {"runtimePath", privateRuntime ? "private" : "evaluation"},
                        {"requestedProvider", requestedProvider}, {"observedProviderAvailable", providerObserved},
                        {"providerIdentityVerified", false}, {"providerObservation", output.string() + ".provider.json"},
                        {"upstreamPort", upstreamPort}, {"profileId", profile.id},
                        {"profileDigest", revia::audit::ContentDigest(authored.dump())},
                        {"corpusDigest", revia::audit::ContentDigest(corpusBytes)}, {"seeds", cognitionSeeds},
                        {"uniqueCases", cognitionCases.size()}, {"expectedRuns", cognitionCases.size() * cognitionSeeds.size()},
                        {"contextSize", settings.contextSize}, {"maxTokens", settings.maxTokens}, {"temperature", profile.temperature},
                        {"profileTemperatureOverride", profile.bHasTemperatureOverride}, {"optionalReview", review},
                        {"completionRequestSeedInjectionEnabled", true}, {"backendSeedVerified", false},
                        {"capturedCorpus", capturedCorpusPath.string()}, {"runtimeDirectory", runtimeDirectory.string()},
                        {"liveQualified", false}, {"independentPersonalityReview", "pending"}}
                            .dump(2) +
                        '\n');
                return result;
            }
            auto report = revia::evaluation::ConversationEvaluator::Run(
                cases, [&](const std::string& input, const std::vector<conversationMessage>& prior)
                { return runtime.EvaluateTurn(input, prior, profile, true); }, settings.modelName);
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
            WriteEvidence(output.string() + ".review.json", revia::quality::BuildSemanticReview(corpus, report).dump(2) + '\n');
            WriteEvidence(output.string() + ".metadata.json",
                nlohmann::json{{"provider", settings.modelName}, {"host", "127.0.0.1"}, {"upstreamPort", upstreamPort},
                    {"observedPort", observed.port}, {"routerPort", settings.port}, {"profileId", profile.id},
                    {"profileDigest", revia::audit::ContentDigest(authored.dump())},
                    {"profileTemperatureOverride", profile.bHasTemperatureOverride}, {"temperature", profile.temperature},
                    {"maxTokens", settings.maxTokens}, {"contextSize", settings.contextSize}, {"optionalReview", review},
                    {"runtimeDirectory", runtimeDirectory.string()}, {"capturedCorpus", capturedCorpusPath.string()},
                    {"requests", output.string() + ".requests.json"}, {"responses", output.string() + ".responses.json"},
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
