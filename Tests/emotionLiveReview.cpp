#include "reviaSessionTestAccess.h"

#include <fstream>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>

namespace
{
using namespace revia;
using namespace revia::runtime;
using Access = ReviaSessionTestAccess;
using tests::Check;
using json = nlohmann::json;

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& root)
        : previous(std::filesystem::current_path()) { std::filesystem::current_path(root); }
    ~WorkingDirectory() { std::filesystem::current_path(previous); }
private:
    std::filesystem::path previous;
};
}
void RunEmotionOwnershipLive(const std::string& runtimeDirectory, const std::string& reportPath)
{
    const auto root = std::filesystem::absolute(runtimeDirectory);
    const auto output = std::filesystem::absolute(reportPath);
    Check(!std::filesystem::exists(output), "A live trace must use a new output file.");
    Check(!std::filesystem::exists(root / "RuntimeData"), "Live review requires a fresh runtime data directory.");
    WorkingDirectory cwd(root);
    std::ifstream settingsFile(root / "Config/settings.json");
    const auto configuration = json::parse(settingsFile);
    const auto& llm = configuration.at("llm");
    Check(llm.value("backend", "") == "LLamaCpp" && llm.value("host", "") == "127.0.0.1" &&
        llm.value("autoStartServer", false) && llm.value("shutdownServerOnExit", false) &&
        !llm.value("visionEnabled", true), "Live review requires an explicitly configured owned local text model.");
    Check(!configuration.at("intelligence").value("enabled", true), "Live review uses the main model only.");
    for (const std::string section : {"speech", "speechRecognition", "vision", "perception", "initiative", "embedding"})
        Check(!configuration.at(section).value("enabled", true), "Disable unrelated live-review service: " + section);
    Check(!configuration.at("presence").value("externalAdaptersEnabled", true) &&
        !configuration.at("conversation").value("archiveEnabled", true),
        "Live review must use isolated private dialogue without external adapters or archive writes.");
    Check(configuration.at("activeProfile") == "review", "Live review must use its explicit review profile.");
    std::ifstream profileFile(root / "Config/Profiles/review.json");
    const auto profile = json::parse(profileFile);
    Check(!profile.value("memoryEnabled", true), "Live review must not retrieve or classify private curated memory.");
    std::ofstream report(output);
    Check(report.good(), "Could not open the new live trace.");
    ReviaSession session;
    Check(session.Start(), "The live review session failed to start.");
    httplib::Client health("127.0.0.1", llm.at("port").get<int>());
    health.set_connection_timeout(2, 0);
    health.set_read_timeout(2, 0);
    const auto ready = health.Get("/health");
    const auto models = health.Get("/v1/models");
    Check(ready && ready->status == 200 && models && models->status == 200,
        "Live review did not verify the actual model health and identity endpoints.");
    const auto catalogue = json::parse(models->body);
    bool expectedModel = false;
    for (const auto& model : catalogue.at("data"))
        expectedModel = expectedModel || model.value("id", "") == llm.value("modelName", "");
    Check(expectedModel, "Live review endpoint reported a different model.");
    const auto state = [&]
    {
        const auto current = Access::Emotions(session).Current();
        json readings = json::object();
        for (const auto& reading : current.emotion.Significant(.01F))
            readings[emotion::ToString(reading.emotion)] = reading.value;
        const auto legacy = Access::LegacyAffect(session).Current();
        return json{{"emotion", readings}, {"affect", ToString(current.affect.state)},
            {"intensity", current.affect.intensity}, {"moodValence", current.mood.valence},
            {"moodIrritability", current.mood.irritability}, {"legacyAffect", ToString(legacy.state)},
            {"legacyIntensity", legacy.intensity}};
    };
    report << json{{"kind", "live_model_review"}, {"syntheticInputs", true},
        {"freshIdentity", true}, {"voiceOutput", false}, {"models", catalogue},
        {"initialState", state()}}.dump() << '\n' << std::flush;
    for (const std::string input : {"Hi Revia.", "Are you sad?", "You are useless.",
        "Okay, explain one small thing about maple leaves.", "Thank you, that was helpful.",
        "Are you sad?", "Are you lonely?"})
    {
        const auto before = state();
        std::cout << "[Live review] " << input << '\n' << std::flush;
        const auto started = std::chrono::steady_clock::now();
        const auto result = session.Submit(input);
        const double milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        report << json{{"input", input}, {"before", before}, {"reply", result.text},
            {"succeeded", result.succeeded}, {"reason", result.reason}, {"trace", result.reasoning},
            {"elapsedMs", milliseconds}, {"after", state()}}.dump() << '\n' << std::flush;
        Check(report.good(), "Live review trace could not be persisted.");
        Check(result.succeeded && result.fromAssistant && !result.text.empty(),
            "A live review turn failed; its partial trace was retained.");
    }
    session.Stop();
    report << json{{"kind", "stopped"}, {"state", state()}}.dump() << '\n';
    report.close();
    Check(!report.fail(), "Live trace close failed.");
    std::cout << "Live model trace captured; human personality acceptance remains required.\n";
}
