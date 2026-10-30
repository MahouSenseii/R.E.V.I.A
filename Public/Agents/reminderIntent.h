#pragma once

#include <string>

namespace revia::agents
{
// The current person's raw input only; source/reference blocks and earlier turns
// cannot authorize an effect. Native reminder parsing remains the grammar owner.
[[nodiscard]] bool RequestsReminderEffect(const std::string& input);
}
