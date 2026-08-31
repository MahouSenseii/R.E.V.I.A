#pragma once

#include "Actions/actionTypes.h"

#include <filesystem>
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

} // namespace revia::tests

// Entry points for the split suites, called from the single test main.
void RunEmotionTests();
void RunIdentityTests();
void RunAppraisalTests();
void RunStatePacketTests();
void RunRelationshipTests();
void RunDevelopmentTests();
void RunAutonomyTests();
void RunLoadAndNameTests();
