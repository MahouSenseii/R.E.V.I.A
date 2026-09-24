#pragma once

#include "Goals/goalTypes.h"

#include <filesystem>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace revia::actions::windows
{

// Owns only windows it creates for one rehearsal. It never attaches to or closes a
// pre-existing user window. Supported applications are intentionally explicit.
class DisposableApplicationFixtures
{
public:
    DisposableApplicationFixtures() = default;
    ~DisposableApplicationFixtures();

    DisposableApplicationFixtures(const DisposableApplicationFixtures&) = delete;
    DisposableApplicationFixtures& operator=(const DisposableApplicationFixtures&) = delete;

    [[nodiscard]] bool Launch(
        const std::vector<std::string>& applications,
        const std::filesystem::path& scratchRoot,
        std::string& outError);
    [[nodiscard]] bool Retarget(goals::Goal& goal, std::string& outError) const;
    void Close();

private:
    struct Fixture
    {
        std::string windowTitle;
        std::uintptr_t windowHandle = 0;
        std::uintptr_t processHandle = 0;
    };
    std::map<std::string, Fixture> fixtures;
};

} // namespace revia::actions::windows
