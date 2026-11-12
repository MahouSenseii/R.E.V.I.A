#pragma once

#include "Actions/actionTypes.h"

#include <cstddef>
#include <optional>
#include <string>

namespace revia::agents
{

inline constexpr std::size_t WorkerToolOutputReservation = 16384;
inline constexpr unsigned MaximumWorkerToolsPerAttempt = 4;

// What a worker's tool step may answer: one bounded action, or null to stop choosing.
// With `mayEnd` false, null is left out. That is for a first step, where nothing has been
// read and null ends the work with nothing found.
[[nodiscard]] std::string WorkerToolSchema(bool allowWrites, bool allowProcess, bool mayEnd = true);
[[nodiscard]] bool ParseWorkerToolResponse(const std::string& response, bool allowWrites, bool allowProcess,
    std::optional<actions::ActionRequest>& outAction, std::string& outError);
[[nodiscard]] std::string WorkerToolReceipt(const actions::ActionRequest& request, const actions::ActionOutcome& outcome);

} // namespace revia::agents
