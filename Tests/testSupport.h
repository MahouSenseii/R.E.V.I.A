#pragma once

#include "Actions/actionTypes.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

// Shared harness for the split test files.
//
// foundationTests.cpp grew to six thousand lines with its helpers locked inside an
// anonymous namespace, so a new subsystem could not get its own file without copying
// them. This is the seam that lets the split happen one subsystem at a time instead of
// as one enormous move that would be impossible to review.
namespace revia::tests
{

inline void Check(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// A throwaway directory that removes itself, and refuses to remove anything it did not
// create -- the name and parent are both verified before the recursive delete.
class ScopedTestDirectory
{
public:
    ScopedTestDirectory()
    {
        root = std::filesystem::temp_directory_path() /
            ("revia-subsystem-tests-" + revia::actions::NewActionId());
        std::filesystem::create_directories(root);
    }

    ~ScopedTestDirectory()
    {
        std::error_code error;
        const std::string filename = root.filename().string();
        if (filename.rfind("revia-subsystem-tests-", 0) == 0 &&
            root.parent_path() == std::filesystem::temp_directory_path())
        {
            std::filesystem::remove_all(root, error);
        }
    }

    ScopedTestDirectory(const ScopedTestDirectory&) = delete;
    ScopedTestDirectory& operator=(const ScopedTestDirectory&) = delete;

    std::filesystem::path root;
};

// A short but genuinely playable WAV: RIFF/WAVE, a PCM fmt chunk, and a data chunk
// whose declared samples are really present.
//
// Fixtures used to write the four bytes "RIFF", or a header with an empty data
// chunk, because a clip only had to exist. It has to play now, so a bank cannot
// count a failed render as a rendered sound.
inline void WriteMinimalWav(const std::filesystem::path& path,
    const std::uint32_t sampleBytes = 4)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const auto little = [&file](const std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            file.put(static_cast<char>((value >> shift) & 0xFF));
        }
    };
    file.write("RIFF", 4);
    little(36 + sampleBytes);
    file.write("WAVEfmt ", 8);
    little(16);
    const unsigned char format[16] = {1, 0, 1, 0, 0x44, 0xAC, 0, 0,
        0x88, 0x58, 1, 0, 2, 0, 16, 0};
    file.write(reinterpret_cast<const char*>(format), sizeof(format));
    file.write("data", 4);
    little(sampleBytes);
    for (std::uint32_t index = 0; index < sampleBytes; ++index)
    {
        file.put(static_cast<char>(index % 256));
    }
}

} // namespace revia::tests

// Entry points for the split suites, called from the single test main.
void RunEmotionTests();
void RunEmotionOwnershipTests();
void RunEmotionOwnershipLive(const std::string& runtimeDirectory, const std::string& reportPath);
void RunIdentityTests();
void RunAppraisalTests();
void RunStatePacketTests();
void RunRelationshipTests();
void RunDevelopmentTests();
void RunAutonomyTests();
void RunLoadAndNameTests();
void RunVoicePoolTests();
void RunMicrophoneTests();
void RunActivityExecutionTests();
void RunAuditFindingsTests();
void RunSpeechInterruptionTests();
void RunInvestigationTests();
void RunInvestigationLive(const std::string& host, int port);
void RunSpeechCoordinatorTests();
void RunPresentationTests();
void RunSkillTests();
void RunCoordinationOverheadTests();
void RunSpeechInterruptionLive(const std::filesystem::path& sourceClip);
void RunActionCancellationTests();
void RunActionAuditTests();
void RunDesktopControlTests();
void RunPerformanceTests();
void RunOperatorLoopTests();
void RunDesktopAuthorizationTests();
void RunTargetBindingTests();
void RunActionApprovalTests();
void RunConsequenceCorpusTests();
void RunNativeDesktopTests();
void RunWorkspaceDemonstration();
void RunIdentityPersistenceTests();
void RunIdentityFinalSaveTests();
void RunLearningDurabilityTests();
void RunEmbeddingBackfillTests();
void RunMemoryDedupTests();
void RunBoundedFileReadTests();
void RunSpeakerContinuityTests();
void RunSpeechAttributionTests();
void RunProfileActivationTests();
void RunProactiveStateTests();
void RunIdentityCrashChild(const std::filesystem::path& root);
