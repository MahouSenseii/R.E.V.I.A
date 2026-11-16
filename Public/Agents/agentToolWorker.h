#pragma once

#include "Actions/actionTypes.h"
#include "LLM/toolCall.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace revia::agents
{

inline constexpr std::size_t WorkerToolOutputReservation = 16384;
// How much of a run's arguments its receipt repeats: the command, or a short script given on it.
inline constexpr std::size_t WorkerToolArgumentBytes = 2048;
inline constexpr unsigned MaximumWorkerToolsPerAttempt = 4;
// The checking worker of a run that may change files, when it is to run a check of its own after
// the named test: what it reads first, the test, and the check, which is its last step.
inline constexpr unsigned MaximumCheckingWorkerToolsPerAttempt = 6;
// The worker that may write looks for what to change before it writes: a listing, the test and
// a source file or two, then the write. It counts model replies, as one reply may read several
// files at once.
inline constexpr unsigned MaximumWritingWorkerToolsPerAttempt = 6;
// Its tool calls across those replies, bounded below the workflow's own 16 (20 where the checking
// worker also runs a check of its own) so that the checking worker after it keeps room for its own.
inline constexpr unsigned MaximumWritingWorkerCallsPerAttempt = 12;

// What a worker's tool step may answer: one bounded action, or null to stop choosing.
// With `mayEnd` false, null is left out. That is for a first step, where nothing has been
// read and null ends the work with nothing found.
[[nodiscard]] std::string WorkerToolSchema(bool allowWrites, bool allowProcess, bool mayEnd = true);
[[nodiscard]] bool ParseWorkerToolResponse(const std::string& response, bool allowWrites, bool allowProcess,
    std::optional<actions::ActionRequest>& outAction, std::string& outError);
[[nodiscard]] std::string WorkerToolReceipt(const actions::ActionRequest& request, const actions::ActionOutcome& outcome);

// The worker that may write works in the model's own tool-call format: list_directory,
// read_text_file and write_text_file, each with an absolute `path`, the write with the whole
// new `content`. It never names a digest: the host checks a write against this worker's own
// last read or write of that file.
inline constexpr std::size_t MaximumWritingWorkerContentBytes = 8192;
[[nodiscard]] std::vector<llm::ToolDefinition> WritingWorkerTools();
// One call as an action request, or the reason it cannot be one, said to the model. A write's
// expectedDigest is left "missing" for the host to fill in.
[[nodiscard]] bool ParseWritingWorkerCall(const llm::ToolCall& call, std::optional<actions::ActionRequest>& outAction, std::string& outError);
// What the model is shown for one executed call: a file's text as it is, a listing as one name a
// line (folders end in "/"), a write's outcome in a sentence.
[[nodiscard]] std::string WritingWorkerToolResult(const actions::ActionRequest& request, const actions::ActionOutcome& outcome);

} // namespace revia::agents
