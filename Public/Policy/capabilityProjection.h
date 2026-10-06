#pragma once

#include "Actions/actionTypes.h"

namespace revia::policy
{
// A conservative tool description. Execution still evaluates both original policies.
[[nodiscard]] actions::CapabilitySettings ProjectCapabilityScope(
    const actions::CapabilitySettings& captured, const actions::CapabilitySettings& current);
}
