#include "Improvement/improvementAgent.h"
#include "testSupport.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
using namespace revia::improvement;
using revia::tests::Check;

void WriteSource(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    Check(output.good(), "The reporter fixture could not write its temporary source.");
}

responseOutput ProposalReply(const std::string& path)
{
    responseOutput reply;
    reply.bSuccess = true;
    reply.response = nlohmann::json{{"found", true}, {"title", "Correct the synthetic return value"},
        {"problem", "The synthetic return value is wrong."}, {"reason", "The selected expression returns two."},
        {"evidence", "The source expression returns one."}, {"expected_benefit", "Return the synthetic expected value."},
        {"risks", "No known risk in this disposable fixture."}, {"benefit", 0.9}, {"risk", 0.0}, {"file", path},
        {"find", "    return 1;\n"}, {"replace", "    return 2;\n"}}
                         .dump();
    return reply;
}

struct ReporterFixture
{
    revia::tests::ScopedTestDirectory directory;
    std::filesystem::path source = directory.root / "source";
    std::shared_ptr<ProposalStore> store = std::make_shared<ProposalStore>();
    ImprovementAgent agent;
    std::uint64_t audienceRevision = 1;
    int captures = 0;
    int fallbackReports = 0;
    std::vector<std::uint64_t> admittedReports;

    ReporterFixture()
    {
        WriteSource(source / "CMakeLists.txt", "project(ReporterFixture)\n");
        WriteSource(source / "Private/Speech/first.cpp", "int First()\n{\n    return 1;\n}\n");
        WriteSource(source / "Private/Speech/second.cpp", "int Second()\n{\n    return 1;\n}\n");
        std::filesystem::create_directories(source / "Public");
        std::string error;
        Check(store->Initialize(directory.root / "proposals", error), error);
    }

    ImprovementAgent::Dependencies Dependencies()
    {
        ImprovementAgent::Dependencies result;
        result.catalog = SourceCatalog(source);
        result.store = store;
        result.report = [this](const CodeProposal&, const std::string&) { ++fallbackReports; };
        return result;
    }

    ImprovementAgent::ReporterFactory CaptureReporter()
    {
        return [this]
        {
            ++captures;
            return [this, capturedAudience = audienceRevision](const CodeProposal&, const std::string&)
            {
                if (capturedAudience == audienceRevision)
                    admittedReports.push_back(capturedAudience);
            };
        };
    }

    static ReviewJob Job(const std::string& path)
    {
        ReviewJob job;
        job.trigger = "request";
        job.file = path;
        job.request = "Review the exact synthetic expression.";
        return job;
    }
};

void TestReporterCapturesBeforeReviewAndRefreshesForNextRun()
{
    ReporterFixture fixture;
    auto dependencies = fixture.Dependencies();
    dependencies.captureReporter = fixture.CaptureReporter();
    int reviews = 0;
    int capturesAtFirstReview = 0;
    dependencies.review = [&](const std::string&, const std::string&, const std::string&, std::stop_token)
    {
        if (++reviews == 1)
        {
            capturesAtFirstReview = fixture.captures;
            ++fixture.audienceRevision;
            return ProposalReply("Private/Speech/first.cpp");
        }
        return ProposalReply("Private/Speech/second.cpp");
    };
    improvementSettings settings;
    settings.bVerify = false;
    fixture.agent.Configure(settings, std::move(dependencies));
    const auto stale = fixture.agent.Run(ReporterFixture::Job("Private/Speech/first.cpp"), {});
    Check(stale.kind == ReviewOutcome::Kind::Proposed && stale.proposal, "The stale reporter fixture did not produce a proposal.");
    Check(fixture.fallbackReports == 0 && fixture.admittedReports.empty(),
        "An old private review used the fallback reporter after its captured audience changed.");
    Check(capturesAtFirstReview == 1 && fixture.captures == 1, "The reporter was not captured exactly once before review work.");
    const auto fresh = fixture.agent.Run(ReporterFixture::Job("Private/Speech/second.cpp"), {});
    Check(fresh.kind == ReviewOutcome::Kind::Proposed && fixture.captures == 2 && fixture.fallbackReports == 0 &&
              fixture.admittedReports == std::vector<std::uint64_t>{2},
        "A new review did not capture and report under its fresh audience.");
}

void TestReporterFallbackAndEmptyCapturedReporter()
{
    for (const bool configuredFactory : {false, true})
    {
        ReporterFixture fixture;
        auto dependencies = fixture.Dependencies();
        dependencies.review = [](const std::string&, const std::string&, const std::string&, std::stop_token)
        { return ProposalReply("Private/Speech/first.cpp"); };
        if (configuredFactory)
        {
            dependencies.captureReporter = [&]() -> ImprovementAgent::Reporter
            {
                ++fixture.captures;
                return {};
            };
        }
        improvementSettings settings;
        settings.bVerify = false;
        fixture.agent.Configure(settings, std::move(dependencies));
        const auto result = fixture.agent.Run(ReporterFixture::Job("Private/Speech/first.cpp"), {});
        Check(result.kind == ReviewOutcome::Kind::Proposed, "The fallback reporter fixture did not produce a proposal.");
        Check(fixture.fallbackReports == (configuredFactory ? 0 : 1) && fixture.captures == (configuredFactory ? 1 : 0),
            "The optional factory changed legacy reporting or an empty captured reporter fell back to current disclosure.");
    }
}

void TestProofRetainsTheAttemptReporter()
{
    for (const bool deferredProof : {false, true})
    {
        ReporterFixture fixture;
        auto dependencies = fixture.Dependencies();
        dependencies.captureReporter = fixture.CaptureReporter();
        dependencies.review = [](const std::string&, const std::string&, const std::string&, std::stop_token)
        { return ProposalReply("Private/Speech/first.cpp"); };
        bool invalidateDuringProof = true;
        int builds = 0;
        int capturesAtFirstBuild = 0;
        dependencies.workbench = std::make_shared<Workbench>(
            fixture.source, fixture.directory.root / "proof",
            [&](const std::filesystem::path&, const std::filesystem::path&, std::stop_token)
            {
                if (++builds == 1)
                    capturesAtFirstBuild = fixture.captures;
                if (invalidateDuringProof)
                {
                    invalidateDuringProof = false;
                    ++fixture.audienceRevision;
                }
                BuildOutcome outcome;
                outcome.completed = outcome.built = outcome.testsRan = true;
                outcome.scriptExitCode = outcome.discoveryExitCode = outcome.testExitCode = 0;
                outcome.discoveryOutput = R"({"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[{"name":"Fixture.Reporter"}]})";
                outcome.testOutput = "1/1 Test #1: Fixture.Reporter ........ Passed 0.01 sec\n";
                return outcome;
            },
            [](const CodeChange&, const std::string&, const std::string&) { return std::string{}; });
        improvementSettings settings;
        settings.bVerify = !deferredProof;
        auto job = ReporterFixture::Job("Private/Speech/first.cpp");
        fixture.agent.Configure(settings, dependencies);
        if (deferredProof)
        {
            const auto drafted = fixture.agent.Run(job, {});
            Check(drafted.kind == ReviewOutcome::Kind::Proposed && drafted.proposal,
                "The deferred reporter fixture did not store its original proposal.");
            job.trigger = "prove";
            job.taskId = drafted.proposal->id;
            fixture.captures = fixture.fallbackReports = 0;
            fixture.admittedReports.clear();
            settings.bVerify = true;
            fixture.agent.Configure(settings, dependencies);
        }
        const auto proved = fixture.agent.Run(job, {});
        Check(proved.kind == ReviewOutcome::Kind::Proposed && proved.proposal && proved.proposal->status == ProposalStatus::Verified &&
                  builds >= 1,
            "The scripted workbench did not exercise actual proof reporting: " +
                (proved.proposal ? proved.proposal->verificationSummary : proved.note) + "; builds=" + std::to_string(builds));
        Check(capturesAtFirstBuild == 1 && fixture.captures == 1 && fixture.fallbackReports == 0 && fixture.admittedReports.empty(),
            "Proof reloaded the fallback reporter or captured a new audience after work had already begun.");
        job.trigger = "prove";
        job.taskId = proved.proposal->id;
        const auto freshProof = fixture.agent.Run(job, {});
        Check(freshProof.proposal && freshProof.proposal->status == ProposalStatus::Verified && fixture.captures == 2 &&
                  fixture.fallbackReports == 0 && fixture.admittedReports == std::vector<std::uint64_t>{2},
            "A fresh proof did not capture its own admitted reporter.");
    }
}
}

void RunImprovementAgentTests()
{
    TestReporterCapturesBeforeReviewAndRefreshesForNextRun();
    TestReporterFallbackAndEmptyCapturedReporter();
    TestProofRetainsTheAttemptReporter();
    std::cout << "Improvement reporter checks passed: attempt capture, stale disclosure refusal and legacy fallback.\n";
}
