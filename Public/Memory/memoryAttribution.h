#pragma once

#include "Agents/responseProvenance.h"

#include <string>

namespace revia::memory
{

// The categories that describe Revia rather than the person she is talking to.
//
// Named here rather than inside the classifier, because two separate rules need the
// same set and a second copy of it would be the kind of quiet disagreement this audit
// keeps finding: one list gains a category, the other does not, and the rule that was
// supposed to guard it silently stops covering it.
[[nodiscard]] bool IsSelfMemoryCategory(const std::string& category);

// Whether a classified memory in this category may be attributed to Revia, given how
// the reply it came from was arrived at.
//
// The structured validation that follows still applies -- this is a gate, not a
// substitute for it. What this settles is only the question the text cannot answer:
// "I hate jazz." reads the same whether she volunteered it or was told to say it, and
// only the runtime knows which happened.
[[nodiscard]] bool AttributableToRevia(
    agents::ResponseProvenance provenance,
    const std::string& category);

} // namespace revia::memory
