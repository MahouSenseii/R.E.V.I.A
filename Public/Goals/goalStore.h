#pragma once

#include "Goals/goalTypes.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace revia::goals
{

// Persists every goal transition for resumption. Action audit remains append-only
// and separate; steps/attempts are relational, variable payloads JSON.
class GoalStore
{
public:
    explicit GoalStore(std::string path = "Goals/revia_goals.db");

    [[nodiscard]] bool Save(const Goal& goal) const;
    [[nodiscard]] std::optional<Goal> Load(const std::string& goalId) const;
    [[nodiscard]] std::vector<Goal> LoadResumable() const;
    [[nodiscard]] std::vector<Goal> LoadRecent(std::size_t maxGoals = 25) const;
    [[nodiscard]] bool Remove(const std::string& goalId) const;
    // Deletes every goal, finished or not, last updated longer ago than `age`. Run at
    // startup so the history of what she was asked to do does not grow forever.
    // Returns how many goals were removed.
    std::size_t PruneOlderThan(std::chrono::hours age) const;
    [[nodiscard]] const std::string& Path() const;

private:
    std::string storePath;
};

} // namespace revia::goals
