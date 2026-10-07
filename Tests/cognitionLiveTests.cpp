#include "../Tools/Quality/cognitionLive.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>

namespace
{
using Json = nlohmann::json;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    Require(file.good(), "Expected retained cognition artifact is missing: " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct Directory
{
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("revia-cognition-live-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    Directory()
    {
        std::filesystem::create_directory(path);
    }

    ~Directory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

revia::evaluation::CognitionCase Case(const std::string& id, const std::string& input, const std::string& oracle)
{
    revia::evaluation::CognitionCase item;
    item.id = id;
    item.family = "artifact-retention-fixture";
    item.oracleId = oracle;
    item.expectedJson = R"({"value":7})";
    item.conversation = {id, id, "Fixture accounting only", {{input, {{revia::evaluation::CheckKind::NotEmpty, {}, 0}}}}};
    item.sourceDigest = revia::evaluation::CognitionSourceDigest(item.conversation);
    return item;
}

std::size_t RecordedCases(const std::filesystem::path& path)
{
    std::istringstream file(Bytes(path));
    std::size_t cases = 0;
    std::string line;
    while (std::getline(file, line))
    {
        const auto record = Json::parse(line);
        if (record.at("record") == "case")
        {
            ++cases;
        }
    }
    return cases;
}

void CheckExclusiveArtifactWrite()
{
    Directory directory;
    const auto path = directory.path / "exact-bytes.bin";
    const std::string original("alpha\0beta", 10);
    std::string error;
    Require(revia::evaluation::WriteEvaluationArtifactOnce(path, original, error), "Initial exclusive artifact creation failed.");
    Require(!revia::evaluation::WriteEvaluationArtifactOnce(path, "replacement", error) && !error.empty(),
        "Repeated artifact creation did not refuse overwrite.");
    Require(Bytes(path) == original, "Refused overwrite changed exact retained bytes, including NULs.");
}

void CheckMissingManifestRefusal()
{
    Directory directory;
    revia::quality::ObservedLocalModel observed(1);
    const auto output = directory.path / "missing-manifest.json";
    std::size_t calls = 0;
    const auto runner = [&](const std::string&, const std::vector<conversationMessage>&) -> revia::evaluation::EvaluationReply
    {
        ++calls;
        return {true, R"({"value":7})", R"({"value":7})", {}};
    };
    bool refused = false;
    try
    {
        (void)revia::quality::RunCognitionLive(
            {Case("fixture-missing-manifest", "source", "json-exact-v1")}, {11}, output, runner, "fixture-only-no-model", observed);
    }
    catch (const std::runtime_error&)
    {
        refused = true;
    }
    Require(refused && calls == 0 && !std::filesystem::exists(output), "Missing campaign manifest was admitted to the live producer.");
}

void CheckBoundProducerArtifacts()
{
    Directory directory;
    const auto output = directory.path / "bound-fixture.json";
    const std::vector<revia::evaluation::CognitionCase> cases = {Case("bound-fixture", "exact source bytes", "json-exact-v1")};
    const std::vector<std::uint64_t> seeds = {11, 29};
    revia::evaluation::CampaignManifest base;
    base.campaignId = "typed-fixture-campaign-no-model";
    base.commit = std::string(40, 'a');
    base.sourceDigest = std::string(64, 'b');
    base.buildDigest = std::string(64, 'c');
    base.providerDigest = std::string(64, 'd');
    base.providerAvailable = true;
    base.settingsDigest = std::string(64, 'e');
    base.fixtureDigest = std::string(64, 'f');
    base.oracleVersion = "json-exact-v1";
    base.hardware = "typed-fixture-only-no-model";
    base.timingBoundary = "fixture-case-wall-time";
    revia::quality::ObservedLocalModel observed(1);
    std::size_t calls = 0;
    const auto runner = [&](const std::string&, const std::vector<conversationMessage>&) -> revia::evaluation::EvaluationReply
    {
        ++calls;
        return {true, R"({"value":7})", R"({"value":7})", {}};
    };
    Require(revia::quality::RunCognitionLive(cases, seeds, output, runner, "typed-fixture-only-no-model", observed, base, true) == 0,
        "Typed bound fixture producer rejected its valid transport evidence.");
    const auto report = Json::parse(Bytes(output));
    Require(report.at("recordedRuns") == 2 && report.at("invalidBindings") == 0 && report.at("semanticPassed") == 2 &&
                report.at("fixtureOnly") == true && report.at("liveQualified") == false,
        "Bound fixture accounting lost admission or claimed live qualification.");
    for (const auto seed : seeds)
    {
        const auto prefix = output.string() + ".seed-" + std::to_string(seed) + ".jsonl";
        auto expected = base;
        expected.seed = seed;
        Require(Bytes(prefix + ".campaign.json") == revia::evaluation::SerializeCampaignManifest(expected) + '\n',
            "Seed campaign did not preserve the exact frozen base identity.");
        const auto casePrefix = prefix + ".case-1";
        const auto source = Bytes(casePrefix + ".source.txt");
        const auto outcome = Bytes(casePrefix + ".outcome.json");
        Require(source == revia::evaluation::CognitionSourceBytes(cases.front().conversation), "Retained source bytes were altered.");
        revia::core::TaskContract task;
        Require(static_cast<bool>(revia::core::DeserializeTaskContract(Bytes(casePrefix + ".task.json"), task)),
            "Retained typed task contract is invalid.");
        revia::core::ContractValidation validation;
        const auto bundle = revia::core::DeserializeEvidenceBundle(Bytes(casePrefix + ".bundle.json"), validation);
        Require(bundle.has_value() && bundle->references().size() == 2, "Retained immutable evidence bundle is invalid.");
        const auto review = Json::parse(Bytes(casePrefix + ".review.json"));
        Require(review.at("campaignDigest") == revia::audit::ContentDigest(revia::evaluation::SerializeCampaignManifest(expected)) &&
                    review.at("evidenceBundle") == casePrefix + ".bundle.json",
            "Review sidecar lost campaign or evidence binding.");
        Require(task.stamp.companionId == "cognition-evaluation" && task.stamp.sessionId == base.campaignId &&
                    task.stamp.taskId == cases.front().id && task.stamp.attemptId == std::to_string(seed) &&
                    task.scope.participantId.empty() && task.scope.audience.kind == revia::identity::AudienceKind::Unknown &&
                    task.scope.audience.audienceId == "cognition-evaluation",
            "Host task changed the frozen identity or attributed system evidence.");
        for (const auto& reference : bundle->references())
        {
            const auto bytes = Bytes(std::filesystem::path(reference.sourceLocator));
            Require(reference.digest == revia::audit::ContentDigest(bytes) && reference.observedAtUnixMs > 0 &&
                        revia::core::SameRuntimeStamp(reference.stamp, task.stamp) &&
                        revia::core::SameMemoryScope(reference.scope, task.scope),
                "Reference digest, original observation time, stamp or scope differs from retained bytes.");
            Require(
                reference.sourceId == "cognition-source" ? bytes == source : reference.sourceId == "cognition-output" && bytes == outcome,
                "Reference points to substituted source or outcome bytes.");
        }
        Require(review.at("sourceDigest") == revia::audit::ContentDigest(source) &&
                    review.at("outputDigest") == revia::audit::ContentDigest(outcome),
            "Canonical byte digests disagree with the verdict.");
    }
    std::map<std::filesystem::path, std::string> retained;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path))
        retained.emplace(entry.path(), Bytes(entry.path()));
    base.buildDigest = std::string(64, '0');
    bool refused = false;
    try
    {
        (void)revia::quality::RunCognitionLive(cases, seeds, output, runner, "typed-fixture-only-no-model", observed, base, true);
    }
    catch (const std::runtime_error&)
    {
        refused = true;
    }
    Require(refused && calls == 2, "Changed campaign overwrote existing evidence or ran another case.");
    for (const auto& [path, bytes] : retained)
        Require(Bytes(path) == bytes, "Repeated campaign invocation changed an immutable artifact.");
    observed.Close();
}

void CheckCaseRetentionAndIncompleteAccounting()
{
    Directory directory;
    const auto output = directory.path / "fixture-aggregate.json";
    const std::vector<revia::evaluation::CognitionCase> cases = {Case("fixture-stable", "stable", "json-exact-v1"),
        Case("fixture-exception", "throw", "json-exact-v1"), Case("fixture-unavailable", "unavailable", "json-exact-v1"),
        Case("fixture-unknown-oracle", "unknown", "fixture-unknown-oracle")};
    const std::vector<std::uint64_t> seeds = {11, 29};
    // The proxy receives no inference traffic: canned replies exercise artifact accounting only.
    revia::quality::ObservedLocalModel observed(1);
    std::map<std::filesystem::path, std::string> firstCase;
    const auto verifyFirstCase = [&]
    {
        for (const auto& [path, bytes] : firstCase)
        {
            Require(Bytes(path) == bytes, "Later cases changed an earlier retained artifact.");
        }
    };
    std::size_t runnerCalls = 0;
    const auto runner = [&](const std::string& input, const std::vector<conversationMessage>&) -> revia::evaluation::EvaluationReply
    {
        ++runnerCalls;
        verifyFirstCase();
        if (input == "throw")
        {
            if (firstCase.empty())
            {
                const auto prefix = output.string() + ".seed-11.jsonl.case-1";
                for (const auto* suffix : {".jsonl", ".review.json", ".requests.json", ".responses.json"})
                {
                    const auto path = std::filesystem::path(prefix + suffix);
                    firstCase.emplace(path, Bytes(path));
                }
            }
            throw std::runtime_error("Fixture runner exception.");
        }
        if (input == "unavailable")
        {
            return {false, {}, {}, "Fixture provider unavailable."};
        }
        return {true, R"({"value":7})", R"({"value":7})", {}};
    };
    Require(revia::quality::RunCognitionLive(cases, seeds, output, runner, "fixture-only-no-model", observed, std::nullopt, true) == 2,
        "Unavailable fixture slots were not surfaced in the live helper exit status.");
    Require(runnerCalls == 8, "Exception or unavailable case discarded later frozen slots.");
    verifyFirstCase();
    const auto aggregate = Json::parse(Bytes(output));
    Require(aggregate.at("uniqueCases") == 4 && aggregate.at("expectedRuns") == 8 && aggregate.at("recordedRuns") == 8 &&
                aggregate.at("missingRuns") == 0 && aggregate.at("unavailable") == 4,
        "Final aggregate omitted exception/unavailable case denominators.");
    Require(aggregate.at("seedArtifacts").size() == 2 && aggregate.at("liveQualified") == false &&
                aggregate.at("independentPersonalityReview") == "pending",
        "Fixture result claimed live or personality qualification.");
    for (const auto seed : seeds)
    {
        const auto prefix = output.string() + ".seed-" + std::to_string(seed) + ".jsonl";
        Require(RecordedCases(prefix) == 4, "Final seed report omitted a frozen case.");
        const auto reviews = Json::parse(Bytes(prefix + ".review.json"));
        Require(reviews.at("runs").size() == 4 && reviews.at("independentReview") == "pending", "Seed review accounting changed.");
        for (std::size_t index = 0; index < cases.size(); ++index)
        {
            const auto casePrefix = prefix + ".case-" + std::to_string(index + 1);
            Require(RecordedCases(casePrefix + ".jsonl") == 1, "Per-case raw report lost or mixed case identity.");
            const auto review = Json::parse(Bytes(casePrefix + ".review.json"));
            Require(review.at("caseId") == cases[index].id && review.at("seed") == seed &&
                        review.at("sourceDigest") == cases[index].sourceDigest &&
                        review.at("outputDigest").get<std::string>().size() == 64 &&
                        review.at("criterionDigest").get<std::string>().size() == 64,
                "Per-case review did not retain output/source/criterion binding.");
            Require(review.at("semanticPassed").is_null() && review.at("personalityPassed").is_null() && review.at("reviewerId") == "",
                "Unreviewed fixture output became an independent human verdict.");
            Require(Json::parse(Bytes(casePrefix + ".requests.json")).empty() && Json::parse(Bytes(casePrefix + ".responses.json")).empty(),
                "Canned runner manufactured inference traffic.");
            if (index == 1 || index == 2)
            {
                Require(review.at("available") == false, "Thrown or unavailable runner result became available.");
            }
            if (index == 3)
            {
                Require(review.at("available") == true && review.at("deterministicSemanticVerdict").is_null(),
                    "Unknown oracle acquired a semantic verdict.");
            }
        }
    }
    std::map<std::filesystem::path, std::string> retained;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path))
    {
        retained.emplace(entry.path(), Bytes(entry.path()));
    }
    bool refused = false;
    try
    {
        (void)revia::quality::RunCognitionLive(cases, seeds, output, runner, "fixture-only-no-model", observed, std::nullopt, true);
    }
    catch (const std::runtime_error&)
    {
        refused = true;
    }
    Require(refused && runnerCalls == 8, "Second invocation did not refuse retained evidence before running cases.");
    for (const auto& [path, bytes] : retained)
    {
        Require(Bytes(path) == bytes, "Second invocation changed a retained case, seed, progress or aggregate artifact.");
    }
    observed.Close();
}

}

void RunCognitionLiveTests()
{
    CheckMissingManifestRefusal();
    CheckBoundProducerArtifacts();
    CheckExclusiveArtifactWrite();
    CheckCaseRetentionAndIncompleteAccounting();
    std::cout << "Cognition live artifact fixtures passed; no model or human review exercised.\n";
}

#ifdef REVIA_COGNITION_LIVE_STANDALONE
int main()
{
    try
    {
        RunCognitionLiveTests();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
#endif
