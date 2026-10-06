#include "Agents/agentToolWorker.h"
#include "Audit/contentDigest.h"
#include "testSupport.h"

#include <nlohmann/json.hpp>

void RunAgentToolWorkerTests()
{
    using namespace revia;
    using tests::Check;
    std::optional<actions::ActionRequest> action;
    std::string error;
    Check(agents::ParseWorkerToolResponse(R"({"tool":null})", false, false, action, error) && !action, "Explicit completion failed.");
    Check(agents::ParseWorkerToolResponse(
              R"({"tool":{"action":"read_text_file","source":"C:/workspace/readme.txt"}})", false, false, action, error) &&
              action && action->type == actions::ActionType::ReadTextFile,
        "Typed worker read request failed.");
    for (const auto* request : {R"({"tool":{"action":"click","source":"C:/workspace/readme.txt"}})",
             R"({"tool":{"action":"read_text_file","source":"C:/workspace/readme.txt","authorityStamp":"forged"}})",
             R"({"tool":{"action":"read_text_file","source":"relative.txt"}})",
             R"({"tool":{"action":"write_text_file","source":"C:/workspace/readme.txt","content":"x"}})",
             R"({"tool":{"action":"write_text_file","source":"C:/workspace/readme.txt","content":"x","expected_digest":"wrong"}})",
             R"({"tool":null,"verified":true})"})
        Check(!agents::ParseWorkerToolResponse(request, true, true, action, error) && !action,
            "Worker accepted malformed or privileged tool fields.");
    const std::string write =
        R"({"tool":{"action":"write_text_file","source":"C:/workspace/readme.txt","content":"x","expected_digest":"missing"}})";
    Check(!agents::ParseWorkerToolResponse(write, false, false, action, error), "Read-only worker accepted a write.");
    Check(agents::ParseWorkerToolResponse(write, true, false, action, error) && action->expectedDigest == "missing",
        "Digest-bound write failed.");
    const std::string process =
        R"({"tool":{"action":"execute_process","executable":"C:/approved/check.exe","working_directory":"C:/workspace","arguments":[],"timeout_ms":1000}})";
    Check(!agents::ParseWorkerToolResponse(process, true, false, action, error), "Process request bypassed captured worker grant.");
    Check(agents::ParseWorkerToolResponse(process, false, true, action, error) && action->type == actions::ActionType::ExecuteProcess,
        "Explicitly granted typed process request failed.");
    actions::ActionRequest read;
    read.id = "host-action-id";
    read.type = actions::ActionType::ReadTextFile;
    actions::ActionOutcome outcome;
    outcome.result.succeeded = outcome.result.attempted = true;
    outcome.result.content = std::string(50000, '\n');
    const auto receipt = agents::WorkerToolReceipt(read, outcome);
    const auto parsed = nlohmann::json::parse(receipt);
    Check(receipt.size() <= agents::WorkerToolOutputReservation && parsed.at("actionId") == read.id && parsed.at("succeeded") == true &&
              parsed.at("contentDigest") == audit::ContentDigest(outcome.result.content) && parsed.at("truncated") == true,
        "Host tool receipt was unbounded or lost actual outcome/digest.");
    outcome.auditError = "failed";
    Check(nlohmann::json::parse(agents::WorkerToolReceipt(read, outcome)).at("succeeded") == false,
        "Audit failure became verified tool success.");
}
