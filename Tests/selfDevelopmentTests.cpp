#include "Improvement/workbench.h"
#include "Improvement/selfDevelopment.h"
#include "Improvement/sourceCatalog.h"
#include "Audit/contentDigest.h"
#include "testSupport.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <memory>

namespace
{
void TestGeneralProofCannotExecuteWithoutHostAdmission()
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("revia-development-admission-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "source/Private");
    std::ofstream(root / "source/Private/sample.cpp") << "int Value() { return 2; }\n";
    const auto sentinel = root / "effect.txt";
    revia::improvement::Workbench bench(root / "source", root / "bench",
        [&](const auto&, const auto&, auto)
        {
            std::ofstream(sentinel) << "executed";
            revia::improvement::BuildOutcome result;
            result.completed = result.built = result.testsRan = true;
            result.scriptExitCode = result.discoveryExitCode = result.testExitCode = 0;
            result.discoveryOutput = R"({"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[{"name":"Fixture"}]})";
            result.testOutput = "1/1 Test #1: Fixture ... Passed 0.01 sec\n";
            return result;
        });
    const auto result = bench.Verify({"Private/sample.cpp", "return 2;", "return 1;"}, {});
    const bool effect = std::filesystem::exists(sentinel);
    std::filesystem::remove_all(root);
    revia::tests::Check(!effect && !result.verified, "General Workbench executed a candidate without trusted host admission.");
}

void TestProtectedSourcesAreRefusedBeforeModelExposure()
{
    for (const auto* path : {"Public/Policy/companionAuthority.h", "Private/Runtime/sessionIdentity.cpp",
             "Private/Audit/actionAuditLogger.cpp", "Private/Improvement/workbench.cpp", "Desktop/reviaWindow.cpp"})
        revia::tests::Check(!revia::improvement::SourceCatalog::IsReviewable(path),
            "Protected source was exposed through ordinary autonomous review: " + std::string(path));
}

using namespace revia::improvement;
using revia::tests::Check;

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

void WriteFile(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << content;
    Check(stream.good(), "Disposable fixture write failed.");
}

struct DevelopmentFixture
{
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("revia-closed-development-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    DevelopmentOptions options;
    DevelopmentDependencies dependencies;
    std::unique_ptr<SelfDevelopment> owner;
    bool denied = false;
    bool regressed = false;
    std::string validator = "fixture-validator-v1";
    std::function<void(DevelopmentStage)> hook;
    std::function<void(DevelopmentValidation&)> alterValidation;
    std::function<void(DevelopmentReview&)> alterReview;
    std::function<void(DevelopmentStage, const DevelopmentCandidate&)> probeHook;
    std::function<void(const std::string&)> sourceReadHook;
    std::string error;

    DevelopmentFixture()
    {
        options.sourceRoot = root / "source";
        options.artifactRoot = root / "artifacts";
        options.configuredIdentity = "companion-fixture";
        options.allowedStages = {DevelopmentStage::Propose, DevelopmentStage::Validate, DevelopmentStage::Review,
            DevelopmentStage::Integrate, DevelopmentStage::Package, DevelopmentStage::Activate, DevelopmentStage::Recover};
        Check(
            revia::audit::ContentDigest(SelfDevelopment::Baseline()) == "a16979155b26c60d47cd4ec514a0b7c6185a0e6b85616542823a41677065fc73",
            "Closed formatter baseline no longer matches independently frozen host bytes.");
        WriteFile(Target(), SelfDevelopment::Baseline());
        dependencies.sourceFingerprint = [this]
        {
            const auto bytes = ReadFile(Target());
            if (sourceReadHook)
                sourceReadHook(bytes);
            return revia::audit::ContentDigest(bytes);
        };
        dependencies.validatorFingerprint = [this] { return revia::audit::ContentDigest(validator); };
        dependencies.registryFingerprint = [] { return revia::audit::ContentDigest(Discovery()); };
        dependencies.admit = [this](const DevelopmentStage stage, const DevelopmentCandidate&)
        {
            if (hook)
                hook(stage);
            return denied ? "Current scoped authority was revoked." : std::string{};
        };
        dependencies.validate = [this](const DevelopmentCandidate& candidate, auto)
        {
            auto value = Validation(candidate, false);
            if (alterValidation)
                alterValidation(value);
            return value;
        };
        dependencies.review = [this](const DevelopmentCandidate& candidate, const ValidationReceipt& receipt, auto)
        {
            DevelopmentReview value{true, candidate.candidateDigest, receipt.digest};
            if (alterReview)
                alterReview(value);
            return value;
        };
        dependencies.releaseProbe = [this](DevelopmentStage stage, const DevelopmentCandidate& candidate, const ValidationReceipt&,
                                        const std::filesystem::path& package)
        {
            Check(ReadFile(package) == candidate.content, "Disposable probe received altered package bytes.");
            if (probeHook)
                probeHook(stage, candidate);
            return Validation(candidate, regressed);
        };
        Restart();
    }

    ~DevelopmentFixture()
    {
        owner.reset();
        std::filesystem::remove_all(root);
    }
    std::filesystem::path Target() const
    {
        return options.sourceRoot / "Desktop/studioDuration.h";
    }
    void Restart()
    {
        owner = std::make_unique<SelfDevelopment>(options, dependencies);
    }
    static std::string Discovery()
    {
        return R"({"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[{"name":"DisposableFormatter"}]})";
    }

    DevelopmentValidation Validation(const DevelopmentCandidate& candidate, const bool failed)
    {
        DevelopmentValidation result;
        result.candidateDigest = candidate.candidateDigest;
        result.sourceDigest = candidate.sourceDigest;
        result.validatorDigest = dependencies.validatorFingerprint();
        result.artifactDigest = revia::audit::ContentDigest("trusted-disposable-fixture-consumer-v1");
        result.objectivePassed = !failed;
        result.objectiveOutput = failed ? "Fresh objective failed." : "Fresh objective passed.";
        result.build.completed = result.build.built = result.build.testsRan = true;
        result.build.scriptExitCode = result.build.discoveryExitCode = 0;
        result.build.testExitCode = failed ? 8 : 0;
        result.build.discoveryOutput = Discovery();
        result.build.testOutput =
            failed ? "1/1 Test #1: DisposableFormatter ... ***Failed 0.01 sec\n" : "1/1 Test #1: DisposableFormatter ... Passed 0.01 sec\n";
        return result;
    }

    DevelopmentCandidate Propose(const int digits = 1)
    {
        DevelopmentOrigin origin{{"companion-fixture", "session-fixture", 1, "task-fixture", "attempt-fixture", 1}, "companion-fixture",
            "fixed-fixture-provider", true};
        DevelopmentCandidate candidate;
        Check(owner->Propose({SelfDevelopment::TargetPath(), "'f', 2)", "'f', " + std::to_string(digits) + ")"}, origin, candidate, error),
            error);
        return candidate;
    }

    DevelopmentCandidate Accept()
    {
        auto candidate = Propose();
        Check(owner->Validate(candidate.id, {}, error), error);
        Check(owner->Review(candidate.id, {}, error), error);
        return candidate;
    }

    DevelopmentCandidate Release()
    {
        auto candidate = Accept();
        Check(owner->Integrate(candidate.id, error), error);
        Check(owner->Package(candidate.id, error), error);
        Check(owner->Activate(candidate.id, error), error);
        return candidate;
    }
};

void TestClosedGrammarRejectsAllOtherEffects()
{
    DevelopmentFixture fixture;
    std::string content;
    int digits = 0;
    for (const CodeChange& change : std::vector<CodeChange>{{"Public/Policy/companionAuthority.h", "'f', 2)", "'f', 1)"},
             {SelfDevelopment::TargetPath(), "'f', 2)", "'f', 0)"}, {SelfDevelopment::TargetPath(), "'f', 2)", "'f', 1); system(\"x\")"},
             {SelfDevelopment::TargetPath(), "#include <QString>", "#include <fstream>"},
             {"Desktop/../Policy/studioDuration.h", "'f', 2)", "'f', 1)"}})
        Check(!SelfDevelopment::ClosedChange(change, content, digits, fixture.error),
            "A change outside the one-literal contract was accepted.");
    auto candidate = fixture.Propose(3);
    Check(candidate.content.find("'f', 3)") != std::string::npos && ReadFile(fixture.Target()) == SelfDevelopment::Baseline(),
        "Proposal altered source or lost the allowed literal.");
    fixture.options.configuredIdentity.clear();
    fixture.Restart();
    DevelopmentCandidate refused;
    Check(!fixture.owner->Propose(candidate.change, candidate.origin, refused, fixture.error),
        "Missing configured origin identity admitted a candidate.");
}

void TestExactLocalLifecyclePreservesUserStateAndQuarantinesFailure()
{
    DevelopmentFixture fixture;
    const auto userFile = fixture.root / "user-private.txt";
    WriteFile(userFile, "earlier state");
    auto candidate = fixture.Release();
    Check(ReadFile(fixture.Target()) == candidate.content && fixture.owner->Snapshot(candidate.id)->activated,
        "Accepted source/package was not activated in the disposable release.");
    Check(!fixture.owner->Recover(candidate.id, fixture.error), "Recovery ran without measured regression.");
    WriteFile(userFile, "new user state");
    fixture.regressed = true;
    Check(fixture.owner->Recover(candidate.id, fixture.error), fixture.error);
    Check(ReadFile(fixture.Target()) == SelfDevelopment::Baseline() && ReadFile(userFile) == "new user state" &&
              fixture.owner->Snapshot(candidate.id)->quarantined &&
              ReadFile(fixture.options.artifactRoot / candidate.id / "component.h") == candidate.content,
        "Recovery lost user data, failed candidate or known-good source.");
    fixture.Restart();
    candidate.origin.stamp.attemptId = "new-attempt";
    DevelopmentCandidate retry;
    Check(!fixture.owner->Propose(candidate.change, candidate.origin, retry, fixture.error),
        "Restart silently re-admitted a quarantined release.");
}

void TestStaleSourceValidatorAndReviewCannotIntegrate()
{
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Accept();
        fixture.validator = "changed validator";
        Check(!fixture.owner->Integrate(candidate.id, fixture.error) && ReadFile(fixture.Target()) == SelfDevelopment::Baseline(),
            "Changed validator reused old acceptance.");
    }
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Accept();
        WriteFile(fixture.Target(), "later user work");
        Check(!fixture.owner->Integrate(candidate.id, fixture.error) && ReadFile(fixture.Target()) == "later user work",
            "Integration overwrote later source work.");
    }
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Propose();
        Check(fixture.owner->Validate(candidate.id, {}, fixture.error), fixture.error);
        fixture.alterReview = [](auto& review) { review.validationDigest = std::string(64, '0'); };
        Check(!fixture.owner->Review(candidate.id, {}, fixture.error) && !fixture.owner->Snapshot(candidate.id)->accepted,
            "Review accepted a different validation receipt.");
    }
}

void TestIncompleteExecutionAndCallbackRevocationAreRefused()
{
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Propose();
        fixture.alterValidation = [](auto& validation) { validation.build.testOutput.clear(); };
        Check(!fixture.owner->Validate(candidate.id, {}, fixture.error) && !fixture.owner->Snapshot(candidate.id)->validation,
            "Empty executed registry certified a candidate.");
    }
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Propose();
        fixture.alterValidation = [&](auto&) { fixture.denied = true; };
        Check(!fixture.owner->Validate(candidate.id, {}, fixture.error) && !fixture.owner->Snapshot(candidate.id)->validation,
            "Validator callback revocation still published trusted evidence.");
    }
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Accept();
        int admissions = 0;
        fixture.hook = [&](auto stage)
        {
            if (stage == DevelopmentStage::Integrate && ++admissions == 2)
                fixture.denied = true;
        };
        Check(!fixture.owner->Integrate(candidate.id, fixture.error) && ReadFile(fixture.Target()) == SelfDevelopment::Baseline(),
            "Admission was not rechecked before the actual integration effect.");
    }
}

void TestStageSeparationStoreFailureAndLegacyRecords()
{
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Propose();
        Check(!fixture.owner->Integrate(candidate.id, fixture.error) && !fixture.owner->Activate(candidate.id, fixture.error),
            "Proposal status implied integration or release authority.");
        fixture.Restart();
        Check(!fixture.owner->Integrate(candidate.id, fixture.error), "Persisted/imported metadata created trusted acceptance.");
    }
    {
        DevelopmentFixture fixture;
        WriteFile(fixture.options.artifactRoot, "unwritable directory substitute");
        DevelopmentOrigin origin{{"companion-fixture", "session-fixture", 1, "task-fixture", "attempt-fixture", 1}, "companion-fixture",
            "fixed-fixture-provider", true};
        DevelopmentCandidate candidate;
        Check(!fixture.owner->Propose({SelfDevelopment::TargetPath(), "'f', 2)", "'f', 1)"}, origin, candidate, fixture.error),
            "Failed private record publication was trusted.");
    }
    {
        DevelopmentFixture fixture;
        fixture.dependencies.admit = {};
        fixture.Restart();
        DevelopmentOrigin origin{{"companion-fixture", "session-fixture", 1, "task-fixture", "attempt-fixture", 1}, "companion-fixture",
            "fixed-fixture-provider", true};
        DevelopmentCandidate candidate;
        Check(!fixture.owner->Propose({SelfDevelopment::TargetPath(), "'f', 2)", "'f', 1)"}, origin, candidate, fixture.error),
            "Default absent admission granted source authority.");
    }
}

void TestPackageDriftAndRecoveryDriftPreserveLaterWork()
{
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Accept();
        Check(fixture.owner->Integrate(candidate.id, fixture.error) && fixture.owner->Package(candidate.id, fixture.error), fixture.error);
        WriteFile(fixture.options.artifactRoot / candidate.id / "component.h", "tampered package");
        Check(!fixture.owner->Activate(candidate.id, fixture.error) && !fixture.owner->Snapshot(candidate.id)->activated,
            "Tampered local release package was activated.");
    }
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Release();
        fixture.regressed = true;
        fixture.hook = [&](auto stage)
        {
            if (stage == DevelopmentStage::Recover)
                WriteFile(fixture.Target(), "later user work");
        };
        Check(!fixture.owner->Recover(candidate.id, fixture.error) && ReadFile(fixture.Target()) == "later user work",
            "Recovery erased later source work.");
    }
}

void TestRecoveryDoesNotClaimCompleteAfterMidEffectRevocation()
{
    DevelopmentFixture fixture;
    auto candidate = fixture.Release();
    fixture.regressed = true;
    int admissions = 0;
    fixture.hook = [&](auto stage)
    {
        if (stage == DevelopmentStage::Recover && ++admissions == 4)
            fixture.denied = true;
    };
    Check(!fixture.owner->Recover(candidate.id, fixture.error) && ReadFile(fixture.Target()) == SelfDevelopment::Baseline() &&
              !fixture.owner->Snapshot(candidate.id)->recovered &&
              ReadFile(fixture.options.artifactRoot / "active-release.json").find(candidate.id) != std::string::npos,
        "Partial recovery claimed completion after revocation between its two effects.");
}

void TestUnrecordedSourceEffectCannotAdvanceAfterJournalReturns()
{
    DevelopmentFixture fixture;
    auto candidate = fixture.Accept();
    const auto journal = fixture.options.artifactRoot / candidate.id / "receipt.json";
    fixture.sourceReadHook = [&](const std::string& bytes)
    {
        if (bytes == candidate.content && !std::filesystem::is_directory(journal))
        {
            std::filesystem::remove(journal);
            std::filesystem::create_directory(journal);
        }
    };
    Check(!fixture.owner->Integrate(candidate.id, fixture.error) && ReadFile(fixture.Target()) == candidate.content,
        "Source/result-journal failure fixture did not perform the intended bounded effect.");
    fixture.sourceReadHook = {};
    std::filesystem::remove(journal);
    Check(!fixture.owner->Package(candidate.id, fixture.error) &&
              !std::filesystem::exists(fixture.options.artifactRoot / candidate.id / "component.h"),
        "Unrecorded source effect advanced to packaging after journal storage returned.");
}

void TestReleaseProbeDriftCannotOverwriteLaterWork()
{
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Accept();
        Check(fixture.owner->Integrate(candidate.id, fixture.error) && fixture.owner->Package(candidate.id, fixture.error), fixture.error);
        fixture.probeHook = [&](auto, const auto& value)
        { WriteFile(fixture.options.artifactRoot / value.id / "component.h", "later package work"); };
        Check(!fixture.owner->Activate(candidate.id, fixture.error) &&
                  !std::filesystem::exists(fixture.options.artifactRoot / "active-release.json"),
            "Activation reused proof after its release probe changed the package.");
    }
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Accept();
        Check(fixture.owner->Integrate(candidate.id, fixture.error) && fixture.owner->Package(candidate.id, fixture.error), fixture.error);
        fixture.probeHook = [&](auto, const auto&) { WriteFile(fixture.options.artifactRoot / "active-release.json", "later release"); };
        Check(!fixture.owner->Activate(candidate.id, fixture.error) &&
                  ReadFile(fixture.options.artifactRoot / "active-release.json") == "later release",
            "Activation overwrote a release published during its probe.");
    }
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Release();
        fixture.regressed = true;
        fixture.probeHook = [&](auto, const auto&) { WriteFile(fixture.options.artifactRoot / "active-release.json", "later release"); };
        Check(!fixture.owner->Recover(candidate.id, fixture.error) && ReadFile(fixture.Target()) == candidate.content &&
                  ReadFile(fixture.options.artifactRoot / "active-release.json") == "later release",
            "Recovery overwrote a release changed during its probe.");
    }
    {
        DevelopmentFixture fixture;
        auto candidate = fixture.Release();
        fixture.regressed = true;
        int admissions = 0;
        fixture.hook = [&](auto stage)
        {
            if (stage == DevelopmentStage::Recover && ++admissions == 4)
                WriteFile(fixture.options.artifactRoot / "active-release.json", "later release");
        };
        Check(!fixture.owner->Recover(candidate.id, fixture.error) && ReadFile(fixture.Target()) == SelfDevelopment::Baseline() &&
                  !fixture.owner->Snapshot(candidate.id)->recovered &&
                  ReadFile(fixture.options.artifactRoot / "active-release.json") == "later release",
            "Recovery overwrote a release changed between its two effects.");
    }
}
}

void RunSelfDevelopmentTests()
{
    TestGeneralProofCannotExecuteWithoutHostAdmission();
    std::cout << "PASS general proof refuses missing host admission\n";
    TestProtectedSourcesAreRefusedBeforeModelExposure();
    std::cout << "PASS protected sources refused before model exposure\n";
    TestClosedGrammarRejectsAllOtherEffects();
    std::cout << "PASS closed grammar and configured provenance\n";
    TestExactLocalLifecyclePreservesUserStateAndQuarantinesFailure();
    std::cout << "PASS exact disposable release/recovery retains newer private state and failed package\n";
    TestStaleSourceValidatorAndReviewCannotIntegrate();
    std::cout << "PASS stale source/validator/review receipts refuse integration\n";
    TestIncompleteExecutionAndCallbackRevocationAreRefused();
    std::cout << "PASS complete execution and final live admission\n";
    TestStageSeparationStoreFailureAndLegacyRecords();
    std::cout << "PASS stages/default denial/durable store/restart fail closed\n";
    TestPackageDriftAndRecoveryDriftPreserveLaterWork();
    std::cout << "PASS package/recovery drift preserves later work\n";
    TestRecoveryDoesNotClaimCompleteAfterMidEffectRevocation();
    std::cout << "PASS partial recovery reports completed source effect without false completion\n";
    TestUnrecordedSourceEffectCannotAdvanceAfterJournalReturns();
    std::cout << "PASS unrecorded source effect refuses automatic advancement\n";
    TestReleaseProbeDriftCannotOverwriteLaterWork();
    std::cout << "PASS release probe and per-effect drift preserve later work\n";
}
