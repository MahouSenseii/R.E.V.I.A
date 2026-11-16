#pragma once

#include "Actions/actionTypes.h"
#include "LLM/toolCall.h"

#include <cstddef>
#include <filesystem>
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
// the named test: its replies (what it reads first, the test, and the check, which is its last
// step) and its tool calls across them. Live, a 9B read the listing and four files a reply each
// before the test, and with six replies had one left.
inline constexpr unsigned MaximumCheckingWorkerToolsPerAttempt = 8;
inline constexpr unsigned MaximumCheckingWorkerCallsPerAttempt = 10;
// The worker that may write looks for what to change before it writes: a listing, the test and
// a source file or two, then the write. It counts model replies, as one reply may read several
// files at once.
inline constexpr unsigned MaximumWritingWorkerToolsPerAttempt = 6;
// Its tool calls across those replies, bounded below the workflow's own 16 (22 where the checking
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


// The checking worker that also runs a check of its own works in the model's own tool-call format
// too: list_directory and read_text_file as above, run_program with the arguments that follow the
// program the owner named, and run_check with a short Python script of its own. Through the JSON
// step, with nowhere to say what it expected before the tool, a 35B-A3B's own check failed on 4 or
// 5 of 13 right fixes and a 9B's on 3 of 6; in this format, saying first what each call must return,
// on 3 of 26 and 2 of 18, and on 9 of 12 and 6 of 12 wrong fixes.
inline constexpr std::size_t MaximumCheckingWorkerScriptBytes = 4096;
[[nodiscard]] std::vector<llm::ToolDefinition> CheckingWorkerTools();
// One call as an action request, or the reason it cannot be one, said to the model. run_program
// runs `program` and run_check runs `python` with -c and the script, both in `folder` and for at
// most `timeoutMs`. `outOwnCheck` says which of the two a run is.
[[nodiscard]] bool ParseCheckingWorkerCall(const llm::ToolCall& call, const std::filesystem::path& program, const std::filesystem::path& python,
    const std::filesystem::path& folder, int timeoutMs, std::optional<actions::ActionRequest>& outAction, bool& outOwnCheck,
    std::string& outError);
// What the model is shown for one executed call: for a run, how it ended and what it printed.
[[nodiscard]] std::string CheckingWorkerToolResult(const actions::ActionRequest& request, const actions::ActionOutcome& outcome);
// Whether a failed run of the worker's own script failed on the script itself and so checked
// nothing: a NameError, SyntaxError or failed import raised in the script's own lines, not in the
// code it imported. Live, a 9B's check of a right fix used Decimal two lines before importing it.
// An assertion that fails, or any error raised inside the checked code, is a finding and not this.
[[nodiscard]] bool CheckScriptItselfFailed(const process::ProcessResult& result);
// How many of its own scripts may fail so before the worker is no longer asked for a check.
inline constexpr unsigned MaximumBrokenCheckScripts = 2;

} // namespace revia::agents
