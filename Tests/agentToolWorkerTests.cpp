#include "Agents/agentToolWorker.h"
#include "Audit/contentDigest.h"
#include "testSupport.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <utility>
#include <vector>

void RunAgentToolWorkerTests()
{
    using namespace revia;
    using tests::Check;
    std::optional<actions::ActionRequest> action;
    std::string error;
    // A first step may be asked for an action and nothing else: a live 9B answered tool:null
    // to one 20 times in 20, and the work ended with nothing read.
    const auto mayAnswerNull = [](const std::string& schema)
    {
        const auto choices = nlohmann::json::parse(schema).at("properties").at("tool").at("anyOf");
        return std::any_of(choices.begin(), choices.end(), [](const auto& choice) { return choice.value("type", "") == "null"; });
    };
    for (const bool writes : {false, true})
        for (const bool process : {false, true})
        {
            Check(mayAnswerNull(agents::WorkerToolSchema(writes, process)), "A later tool step can no longer stop choosing.");
            const auto first = nlohmann::json::parse(agents::WorkerToolSchema(writes, process, false)).at("properties").at("tool").at("anyOf");
            Check(!mayAnswerNull(agents::WorkerToolSchema(writes, process, false)) &&
                      first.size() + 1 == nlohmann::json::parse(agents::WorkerToolSchema(writes, process)).at("properties").at("tool").at("anyOf").size(),
                "A first tool step that must act can still answer null, or lost one of its actions.");
        }
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
    outcome.auditError.clear();
    outcome.runtimeError = "observer failed";
    Check(nlohmann::json::parse(agents::WorkerToolReceipt(read, outcome)).at("succeeded") == false,
        "A runtime reporting failure became verified tool success.");
    outcome.runtimeError.clear();
    outcome.result.content.clear();
    actions::ActionRequest listing = read;
    listing.type = actions::ActionType::ListDirectory;
    listing.source = actions::Utf8ToPath("C:/workspace");
    actions::ActionOutcome listingOutcome;
    listingOutcome.result.succeeded = listingOutcome.result.attempted = true;
    for (const bool hasEntries : {true, false})
    {
        listingOutcome.result.entries = hasEntries ? std::vector<std::string>{"[FILE]  example.txt"} : std::vector<std::string>{};
        const auto encoded = agents::WorkerToolReceipt(listing, listingOutcome);
        const auto listed = nlohmann::json::parse(encoded);
        Check(listed.at("contentDigest").is_null() && listed.at("output") == (hasEntries ? "[FILE]  example.txt\n" : "") &&
                  listed.at("source") == "C:/workspace" && listed.at("pathDigests").at("source") == audit::ContentDigest("C:/workspace") &&
                  listed.at("succeeded") == true && listed.at("attempted") == true && listed.at("truncated") == false &&
                  encoded.size() <= agents::WorkerToolOutputReservation,
            "A directory receipt claimed a file-content digest or lost its listing, identity, outcome or byte bound.");
    }
    const std::vector<std::string> paths = {"C:/workspace/quote\"and/slash\n.conf",
        "C:/workspace/\xE7\x95\x8C\xF0\x9F\x99\x82.conf", "C:/" + std::string(1500, '\x01')};
    for (const auto& path : paths)
    {
        read.source = actions::Utf8ToPath(path);
        const auto encoded = agents::WorkerToolReceipt(read, outcome);
        const auto identity = nlohmann::json::parse(encoded);
        Check(identity.contains("source") && identity.at("source").is_string() && identity.at("source") == path &&
                  identity.contains("pathDigests") && identity.at("pathDigests").at("source") == audit::ContentDigest(path) &&
                  encoded.size() <= agents::WorkerToolOutputReservation,
            "A complete valid UTF-8 path that fits the receipt was lost, silently shortened or hashed incorrectly.");
    }
    actions::ActionRequest processIdentity;
    processIdentity.id = "host-process-identity";
    processIdentity.type = actions::ActionType::ExecuteProcess;
    const auto longExecutable = "C:/" + std::string(2040, '\x01');
    const auto longDirectory = "C:/" + std::string(2040, '\x02');
    processIdentity.process.executable = actions::Utf8ToPath(longExecutable);
    processIdentity.process.workingDirectory = actions::Utf8ToPath(longDirectory);
    const auto processReceipt = agents::WorkerToolReceipt(processIdentity, outcome);
    const auto identities = nlohmann::json::parse(processReceipt);
    bool unavailable = false;
    for (const auto& [field, value] : std::vector<std::pair<std::string, std::string>>{
             {"executable", longExecutable}, {"working_directory", longDirectory}})
    {
        Check(identities.contains(field) && identities.contains("pathDigests") &&
                  identities.at("pathDigests").at(field) == audit::ContentDigest(value),
            "A process receipt lost the full executable/working-directory identity digest.");
        Check(identities.at(field).is_null() || (identities.at(field).is_string() && identities.at(field) == value),
            "A shortened process path was exposed as a complete usable identity.");
        unavailable = unavailable || identities.at(field).is_null();
    }
    Check(unavailable && processReceipt.size() <= agents::WorkerToolOutputReservation && identities.at("succeeded") == true &&
              identities.at("attempted") == true,
        "Unfittable identity metadata did not become explicitly unavailable within the existing encoded receipt bound.");
}
