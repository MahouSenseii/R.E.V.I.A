#pragma once

#include "Learning/learningRecordStore.h"
#include "Learning/liveCapture.h"
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
    // Exchanges kept from replies the owner judged, newest first, with the corrections saved so
    // far. Listed only for a private audience; otherwise captureStatus says why not.
    std::vector<learning::LiveCaptureSummary> captures;
    std::string captureStatus;
};
}
