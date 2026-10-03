#pragma once

#include "Learning/learningRecordStore.h"
#include "Skills/skillPackage.h"

#include <optional>
#include <string>
#include <vector>

namespace revia::runtime
{
struct LearningStudioSnapshot
{
    std::optional<skills::SkillPackageReference> inventorySkill;
    std::vector<learning::LearningRecord> lessons;
    std::string status;
};
}
