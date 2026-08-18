#pragma once

#include "Goals/goalTypes.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace revia::goals
{

// Durable goal state.
//
// The runner writes here after every transition, so an interrupted goal is
// resumed from its own record rather than replayed from the action audit log.
// The audit log is append-only and has no read-back API by design; it answers
// "what did Revia do", not "where was this goal up to".
//
// Steps and attempts are stored relationally so a single step's evidence can
// be read without loading every goal, while the variable-shaped payloads
// (ActionRequest, budget, spend, capability scope) are stored as JSON columns
// for the same reason the rest of the project uses nlohmann::json for them.
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
