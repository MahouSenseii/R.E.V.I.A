#include "testSupport.h"

#include "Skills/skillPackage.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace
{
using revia::tests::Check;
using namespace revia::skills;

std::filesystem::path Seeds()
{
    auto root = std::filesystem::current_path();
    for (int depth = 0; depth < 4; ++depth)
    {
        if (std::filesystem::exists(root / "Config/Skills/workspace-inventory/1.0.0"))
            return root / "Config/Skills";
        root = root.parent_path();
    }
    return std::filesystem::current_path() / "Config/Skills";
}

std::vector<SkillCase> Fresh(const bool typed)
{
    return {{"fresh-empty", {}, typed ? "0 entries: 0 files, 0 directories, 0 links, 0 other." : "0 entries."},
        {"fresh-link", {"[DIR] projects", "[FILE] notes.txt", "[LINK] shortcut"},
            typed ? "3 entries: 1 files, 1 directories, 1 links, 0 other." : "3 entries."}};
}

SkillPackage Load(const std::string& version)
{
    SkillPackage package;
    std::string error;
    Check(LoadSkillPackage(Seeds() / "workspace-inventory" / version, package, error), error);
    return package;
}

void TestRealSeedsAndImmutableActivePin()
{
    revia::tests::ScopedTestDirectory fixture;
    SkillPackageStore store(fixture.root / "skills", Seeds());
    std::string error;
    Check(store.Initialize(error), error);
    const auto pinned = store.Pin("workspace-inventory");
    Check(pinned && pinned->reference.version == "1.0.0", "The installed update silently replaced baseline task procedure.");
    auto unsafeReference = pinned->reference;
    unsafeReference.id = "../synthetic-outside";
    Check(!store.Activate(unsafeReference, error) && error == "Skill reference is invalid.",
        "Unsafe reference reached seed eligibility instead of input validation.");
    const auto update = Load("1.1.0");
    const auto verification = VerifySkillPackage(update, Fresh(true));
    Check(verification.passed && verification.freshCases == 2, "Fresh held-out cases did not execute successfully.");
    const SkillExportReview review{update.reference.digest, verification.evidenceDigest, true, true, true};
    Check(store.Publish(update, verification, review, error) && store.Activate(update.reference, error), error);
    Check(store.Pin("workspace-inventory")->reference.version == "1.1.0" && pinned->reference.version == "1.0.0",
        "Activation rewrote a captured task pin.");
    const std::vector<std::string> listing{"[FILE]  fixture.txt", "[DIR]   folder"};
    Check(SummarizeInventory(*pinned, listing).summary == "2 entries." &&
              SummarizeInventory(*store.Pin("workspace-inventory"), listing).summary ==
                  "2 entries: 1 files, 1 directories, 0 links, 0 other.",
        "Version selection did not change the actual checked procedure output.");
    Check(store.Rollback("workspace-inventory", error) && store.Pin("workspace-inventory")->reference.digest == pinned->reference.digest,
        error);
    SkillPackageStore restarted(fixture.root / "skills", Seeds());
    Check(restarted.Initialize(error) && restarted.LoadPinned(update.reference)->reference.digest == update.reference.digest &&
              restarted.Pin("workspace-inventory")->reference.version == "1.0.0",
        "Restart lost immutable pin or rollback selection.");
    auto unknown = pinned->reference;
    unknown.digest[0] = unknown.digest[0] == '0' ? '1' : '0';
    Check(!restarted.LoadPinned(unknown), "Missing exact pin silently resolved to latest.");
}

void TestObjectiveChecksExportAndAnotherCompanion()
{
    revia::tests::ScopedTestDirectory fixture;
    SkillPackageStore a(fixture.root / "A", Seeds()), b(fixture.root / "B", Seeds());
    std::string error;
    Check(a.Initialize(error) && b.Initialize(error), error);
    auto package = Load("1.1.0");
    auto verification = VerifySkillPackage(package, Fresh(true));
    SkillExportReview review{package.reference.digest, verification.evidenceDigest, true, false, true};
    Check(!a.Publish(package, verification, review, error), "Parent acceptance overrode a missing disclosure check.");
    auto duplicate = package.cases.front();
    duplicate.id = "different-name-same-case";
    Check(!VerifySkillPackage(package, {duplicate}).passed, "A repeated regression example was counted fresh.");
    auto forged = verification;
    forged.freshEvidence.front().expectedSummary = "Imagined evidence";
    review.disclosureReviewed = true;
    Check(!a.Publish(package, forged, review, error), "Forged case receipt overrode actual objective checks.");
    Check(a.Publish(package, verification, review, error), error);
    const auto exported = fixture.root / "A/exports/workspace-inventory/1.1.0";
    Check(a.Export(package.reference, exported, error), error);
    SkillPackage neutral;
    Check(LoadSkillPackage(exported, neutral, error) && neutral.reference.digest == package.reference.digest, error);
    Check(b.Publish(neutral, verification, review, error) && b.Activate(neutral.reference, error), error);
    Check(SummarizeInventory(*b.Pin("workspace-inventory"), {"[FILE] synthetic-B.txt"}).summary ==
              "1 entries: 1 files, 0 directories, 0 links, 0 other.",
        "Another companion could not use the neutral checked procedure.");
    Check(!std::filesystem::exists(fixture.root / "B/learning") && neutral.procedure.find("synthetic-A-private") == std::string::npos,
        "Neutral import inherited private experience.");
    Check(!a.Export(package.reference, fixture.root / "outside-export", error), "Export escaped its private artifact staging area.");
    Check(RequiresCapability(neutral, revia::actions::ActionType::ListDirectory) &&
              !RequiresCapability(neutral, revia::actions::ActionType::LaunchApplication),
        "Manifest silently authorized undeclared helper capability.");
}

void TestLimitsPrivacyTamperingAndStrictManifest()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto original = Load("1.1.0");
    auto forged = original;
    forged.procedure += "\nprivate: synthetic-A-private relationship: synthetic-person\n";
    Check(!VerifySkillPackage(forged, Fresh(true)).passed, "Private changed procedure retained its accepted digest.");
    const auto partial = SummarizeInventory(original, {"[FILE]  present.txt", "[LIMIT] Additional entries were omitted."});
    Check(partial.verified && !partial.complete && partial.entries == 1 && partial.summary.find("incomplete") != std::string::npos,
        "Bounded directory truncation was falsely reported complete.");
    Check(!SummarizeInventory(original, {"made-up output"}).verified, "Unknown native result was treated as checked evidence.");
    const auto copy = fixture.root / "package";
    std::filesystem::copy(Seeds() / "workspace-inventory/1.1.0", copy, std::filesystem::copy_options::recursive);
    {
        std::ofstream privateProcedure(copy / "SKILL.md", std::ios::app);
        privateProcedure << "\nprivate: synthetic-alias relationship: synthetic-person\n";
    }
    SkillPackage rejected;
    std::string error;
    Check(!LoadSkillPackage(copy, rejected, error),
        "Known private experience passed the export disclosure check under a newly computed digest.");
    {
        std::ofstream restoredProcedure(copy / "SKILL.md", std::ios::trunc);
        restoredProcedure << original.procedure;
    }
    std::ofstream(copy / "manifest.json", std::ios::app) << "\n{}";
    Check(!LoadSkillPackage(copy, rejected, error), "Trailing JSON admitted a package manifest.");
    {
        std::ifstream originalManifest(Seeds() / "workspace-inventory/1.1.0/manifest.json", std::ios::binary);
        std::ofstream restoredManifest(copy / "manifest.json", std::ios::binary | std::ios::trunc);
        restoredManifest << originalManifest.rdbuf();
        Check(originalManifest.good() && restoredManifest.good(), "Synthetic malformed-manifest fixture could not be restored.");
    }
    std::ofstream(copy / "helper.exe") << "synthetic never executed";
    Check(!LoadSkillPackage(copy, rejected, error), "Reading a procedure admitted an unreviewed helper asset.");
}

void TestBroaderRequirementsNeedNewReviewAndFailedSelectionIsAtomic()
{
    revia::tests::ScopedTestDirectory fixture;
    SkillPackageStore store(fixture.root / "skills", Seeds());
    std::string error;
    Check(store.Initialize(error), error);
    const auto baseline = *store.Pin("workspace-inventory");
    const auto update = Load("1.1.0");
    const auto verification = VerifySkillPackage(update, Fresh(true));
    const SkillExportReview review{update.reference.digest, verification.evidenceDigest, true, true, true};
    Check(store.Publish(update, verification, review, error), error);
    const auto broadenedDirectory = fixture.root / "broadened";
    std::filesystem::copy(Seeds() / "workspace-inventory/1.1.0", broadenedDirectory, std::filesystem::copy_options::recursive);
    nlohmann::json manifest;
    {
        std::ifstream input(broadenedDirectory / "manifest.json");
        input >> manifest;
    }
    manifest["version"] = "1.2.0";
    manifest["optionalCapabilities"].push_back("launch_application");
    {
        std::ofstream output(broadenedDirectory / "manifest.json", std::ios::trunc);
        output << manifest.dump(2);
    }
    SkillPackage broader;
    Check(LoadSkillPackage(broadenedDirectory, broader, error), error);
    const auto broaderVerification = VerifySkillPackage(broader, Fresh(true));
    Check(broaderVerification.passed && !store.Publish(broader, broaderVerification, review, error),
        "Broader requirements inherited an earlier version's approval.");
    std::filesystem::create_directory(fixture.root / "skills/selection.json.tmp");
    Check(!store.Activate(update.reference, error) && store.Pin("workspace-inventory")->reference.digest == baseline.reference.digest,
        "Failed selection publication changed the active accepted procedure.");
    SkillPackageStore restored(fixture.root / "skills", Seeds());
    Check(!restored.Initialize(error), "Blocked atomic write falsely reported initialization durable.");
    Check(SummarizeInventory(baseline, {"[FILE] sample.txt"}).summary == "1 entries.", "Failed selection damaged a running pin.");
}
}

void RunSkillPackageTests()
{
    TestRealSeedsAndImmutableActivePin();
    TestObjectiveChecksExportAndAnotherCompanion();
    TestLimitsPrivacyTamperingAndStrictManifest();
    TestBroaderRequirementsNeedNewReviewAndFailedSelectionIsAtomic();
    std::cout << "Skill package checks passed: actual version pins, checked fresh cases, neutral export and rollback.\n";
}
