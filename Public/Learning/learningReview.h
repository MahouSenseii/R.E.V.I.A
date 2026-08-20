#pragma once

#include "Goals/goalTypes.h"
#include "Initiative/attentionPolicy.h"

#include <cstddef>
#include <string>
#include <vector>

namespace revia::learning
{

enum class LessonKind
{
    // Something about how plans are being written, drawn from goal outcomes.
    Planning,
    // Something about whether speaking first is landing, drawn from proposal outcomes.
    Initiative
};

// A candidate conclusion, and the evidence it was drawn from.
//
// Nothing here is applied. A lesson is a sentence Revia would like to remember, offered
// for approval; approving it writes an ordinary memory entry through the ordinary memory
// path. It cannot change a capability, a budget, a policy, or any code.
struct Lesson
{
    std::string id;
    LessonKind kind = LessonKind::Planning;
    std::string statement;
    std::string evidence;
    // How many observations back it. A conclusion from two runs is a coincidence.
    std::size_t sampleSize = 0;
};

[[nodiscard]] std::string ToString(LessonKind value);

// Draws conclusions from recorded outcomes.
//
// Stage 4's last bullet says learning is reviewed memory and measured policy updates, not
// self-modifying executable code. This is the reviewed-memory half, and it is deliberately
// the only half that is inferred: the one policy update in the system -- the initiative
// rate halving below a precision floor -- is computed from counted outcomes inside
// AttentionPolicy, not proposed here. Keeping inference away from the things that grant
// authority is the point.
class LearningReview
{
public:
    // Below this many judged samples a pattern is not worth drawing a conclusion from.
    static constexpr std::size_t MinimumSamples = 4;

    [[nodiscard]] static std::vector<Lesson> Draw(
        const std::vector<goals::Goal>& recentGoals,
        const initiative::InitiativeCounters& proposalCounters);

    // The memory summary an approved lesson becomes. Kept separate so the exact wording
    // that would be written is inspectable before anything is stored.
    [[nodiscard]] static std::string MemorySummary(const Lesson& lesson);
    [[nodiscard]] static std::string MemoryCategory(const Lesson& lesson);
};

} // namespace revia::learning
