#pragma once

#include "Goals/goalTypes.h"

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
    [[nodiscard]] const std::string& Path() const;

private:
    std::string storePath;
};

} // namespace revia::goals
