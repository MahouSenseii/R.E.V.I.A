#pragma once

#include "Goals/goalTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::learning
{

// Procedures that worked, kept so the next similar request starts from one.
//
// The second rung. A goal that ran to the end with its checks passing is a procedure
// worth keeping: the request in the person's words and the steps that did it. When
// a later request reads like one she has done, the procedure goes to the planner as a
// starting point, labelled as her own record and never as the request. A procedure
// that then fails more often than it works stops being offered. Nothing here runs:
// the plan is still written and checked and confirmed the way every plan is.
struct Procedure
{
    std::uint64_t id = 0;
    std::string title;
    std::vector<std::string> steps;
    std::uint32_t successes = 0;
    std::uint32_t failures = 0;
    std::chrono::system_clock::time_point createdAt = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point lastUsedAt = std::chrono::system_clock::now();

    [[nodiscard]] bool Trusted() const { return successes > failures; }
};

class SkillLibrary
{
public:
    static constexpr std::size_t MaximumProcedures = 300;
    static constexpr std::size_t MaximumSteps = 12;
    static constexpr std::size_t LongestStep = 160;
    static constexpr std::size_t LongestTitle = 200;
    // Word overlap below this is a different request.
    static constexpr double MinimumSimilarity = 0.34;

    bool Initialize(const std::filesystem::path& path, std::string& outError);

    // A finished goal. Kept only when it succeeded with at least one step; the same
    // request done again counts as another success. Empty when nothing was kept.
    std::optional<Procedure> Learn(const goals::Goal& finished);
    // A request that failed: the procedures it resembled count a failure each.
    void RecordFailure(const std::string& request);

    // Procedures worth offering for `request`: trusted, most similar first.
    [[nodiscard]] std::vector<Procedure> Similar(const std::string& request, std::size_t maximum = 3) const;
    // The block the planner reads; empty when there is nothing to offer.
    [[nodiscard]] static std::string RenderForPlanner(const std::vector<Procedure>& procedures);

    [[nodiscard]] std::vector<Procedure> Entries() const;
    std::optional<Procedure> Forget(std::size_t number);

    // Word overlap between two requests, 0..1.
    [[nodiscard]] static double Similarity(const std::string& left, const std::string& right);
    // One step as it is kept: what it did and how.
    [[nodiscard]] static std::string DescribeStep(const goals::GoalStep& step);

private:
    bool SaveLocked(std::string& outError) const;

    mutable std::mutex mutex;
    std::filesystem::path path;
    std::vector<Procedure> procedures;
    std::uint64_t nextId = 1;
};

} // namespace revia::learning
