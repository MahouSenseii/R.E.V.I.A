#pragma once

#include "Computer/computerTypes.h"
#include "Computer/taskContent.h"
#include "Goals/goalTypes.h"

namespace revia::computer
{

// The caller supplies an admitted current observation and an independent draft readback.
[[nodiscard]] goals::CompletionEvidence VerifyExactContentPlacement(
    const TaskContent& task, const goals::Goal& goal, const ComputerObservation& observation, bool currentPlacementVerified);

[[nodiscard]] const browser::BrowserElement* MatchOriginalBrowserField(
    const TaskContent& task, const browser::BrowserReceipt& receipt, std::string& refusal);
[[nodiscard]] goals::CompletionEvidence VerifyExactBrowserPlacement(
    const TaskContent& task, const goals::Goal& goal, const browser::BrowserReceipt& receipt);

} // namespace revia::computer
