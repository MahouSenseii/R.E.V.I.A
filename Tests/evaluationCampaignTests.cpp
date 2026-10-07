#include "Evaluation/campaignManifest.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

namespace
{

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

}

void RunEvaluationCampaignTests()
{
    using namespace revia::evaluation;
    const auto fixturePath = std::filesystem::path(REVIA_CAMPAIGN_FIXTURES) / "manifest-mismatches.json";
    std::ifstream input(fixturePath);
    Require(input.good(), "Campaign fixtures must be available.");
    const auto fixtures = nlohmann::json::parse(input);
    CampaignManifest baseline;
    std::string error;
    Require(ParseCampaignManifest(fixtures.at("baseline").dump(), baseline, error), "Independent baseline rejected: " + error);
    Require(fixtures.at("mismatches").size() == 12, "Twelve independent identity mismatches are required.");
    for (const auto& fixture : fixtures.at("mismatches"))
    {
        auto candidateJson = fixtures.at("baseline");
        candidateJson.update(fixture.at("changed"));
        CampaignManifest candidate;
        Require(ParseCampaignManifest(candidateJson.dump(), candidate, error), "Mismatch fixture could not parse: " + error);
        const auto fields = CampaignIdentityMismatches(baseline, candidate);
        Require(
            fields == fixture.at("expected").get<std::vector<std::string>>(), "Identity mismatch was not quarantined by its named field.");
    }
    Require(CampaignIdentityMismatches(baseline, baseline).empty(), "Same campaign rejected.");
    CampaignManifest roundTrip;
    Require(ParseCampaignManifest(SerializeCampaignManifest(baseline), roundTrip, error), "Round-trip failed.");
    Require(CampaignIdentityMismatches(baseline, roundTrip).empty(), "Round-trip changed identity.");
    for (const auto& version : {nlohmann::json(2), nlohmann::json(1.5), nlohmann::json("1")})
    {
        auto invalid = fixtures.at("baseline");
        invalid["schemaVersion"] = version;
        Require(!ParseCampaignManifest(invalid.dump(), roundTrip, error), "Unsupported or noninteger schema version admitted.");
    }
    auto invalid = fixtures.at("baseline");
    invalid["seed"] = -1;
    Require(!ParseCampaignManifest(invalid.dump(), roundTrip, error), "Negative seed admitted.");
    invalid = fixtures.at("baseline");
    invalid["providerAvailable"] = true;
    invalid["providerDigest"] = "";
    Require(!ParseCampaignManifest(invalid.dump(), roundTrip, error), "Available provider without its digest admitted.");
    const auto directory = std::filesystem::temp_directory_path() /
                           ("revia-campaign-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "manifest.json";
    Require(WriteCampaignManifestOnce(path, baseline, error), "Initial immutable write failed: " + error);
    baseline.seed = 42;
    Require(!WriteCampaignManifestOnce(path, baseline, error), "Existing manifest was overwritten.");
    std::ifstream saved(path);
    Require(ParseCampaignManifest(std::string(std::istreambuf_iterator<char>(saved), {}), roundTrip, error) && roundTrip.seed == 17,
        "Refused overwrite changed manifest bytes.");
    saved.close();
    std::filesystem::remove_all(directory);
}

#ifdef REVIA_CAMPAIGN_STANDALONE
int main()
{
    try
    {
        RunEvaluationCampaignTests();
        std::cout << "Campaign manifest checks passed.\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
