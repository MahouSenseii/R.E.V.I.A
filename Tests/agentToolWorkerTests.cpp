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
    {
        // The program is named by `executable`; a repeat of it as the first argument is dropped.
        const auto arguments = [&](const nlohmann::json& given)
        {
            const nlohmann::json tool = {{"action", "execute_process"}, {"executable", "C:/tools/python.exe"},
                {"working_directory", "C:/workspace"}, {"arguments", given}, {"timeout_ms", 3000}};
            std::optional<actions::ActionRequest> parsed;
            std::string why;
            Check(agents::ParseWorkerToolResponse(nlohmann::json{{"tool", tool}}.dump(), false, true, parsed, why) && parsed,
                "A valid process request was refused.");
            return parsed ? parsed->process.arguments : std::vector<std::string>{};
        };
        using Arguments = std::vector<std::string>;
        Check(arguments({"C:/tools/python.exe", "test.py"}) == Arguments{"test.py"} &&
                  arguments({"C:\\Tools\\Python.exe", "test.py"}) == Arguments{"test.py"} &&
                  arguments({"python.exe", "-m", "unittest"}) == (Arguments{"-m", "unittest"}),
            "A program repeated as its own first argument was passed on to it.");
        Check(arguments({"test.py", "python.exe"}) == (Arguments{"test.py", "python.exe"}) && arguments({"test.py"}) == Arguments{"test.py"} &&
                  arguments(nlohmann::json::array()).empty(),
            "Arguments that do not repeat the program were altered.");
    }
    {
        // The digest line a read's receipt ends with is the host's, never file content.
        const std::string digest(64, 'a');
        const auto written = [&](const std::string& content)
        {
            const nlohmann::json tool = {{"action", "write_text_file"}, {"source", "C:/workspace/readme.txt"}, {"content", content},
                {"expected_digest", digest}};
            std::optional<actions::ActionRequest> parsed;
            std::string why;
            Check(agents::ParseWorkerToolResponse(nlohmann::json{{"tool", tool}}.dump(), true, false, parsed, why) && parsed,
                "A valid write with a prior digest was refused.");
            return parsed ? parsed->value : std::string{};
        };
        Check(written("x = 1\nsha256:" + digest) == "x = 1\n" && written("x = 1\nsha256:" + digest + "\n") == "x = 1\n",
            "The digest line copied from a read's receipt was written into the file.");
        const std::string other = "x = 1\nsha256:" + std::string(64, 'b') + "\n";
        Check(written("x = 1\n") == "x = 1\n" && written("see sha256:" + digest) == "see sha256:" + digest && written(other) == other,
            "Content that only resembles the copied digest line was altered.");
    }
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
    {
        // A write's receipt shows what was written, so the worker can tell what its change was.
        actions::ActionRequest write = read;
        write.type = actions::ActionType::WriteTextFile;
        write.value = "def total(values):\n    return sum(values)\n";
        actions::ActionOutcome wrote;
        wrote.result.succeeded = wrote.result.attempted = true;
        const auto done = nlohmann::json::parse(agents::WorkerToolReceipt(write, wrote));
        Check(done.at("written") == write.value && done.at("truncated") == false,
            "A succeeded write's receipt did not carry the content the host wrote.");
        Check(!nlohmann::json::parse(agents::WorkerToolReceipt(read, outcome)).contains("written"),
            "A read's receipt claimed written content.");
        wrote.result.succeeded = false;
        Check(!nlohmann::json::parse(agents::WorkerToolReceipt(write, wrote)).contains("written"),
            "A failed write's receipt claimed its content was written.");
        wrote.result.succeeded = true;
        write.value = std::string(60000, 'x');
        const auto large = agents::WorkerToolReceipt(write, wrote);
        Check(large.size() <= agents::WorkerToolOutputReservation && nlohmann::json::parse(large).at("truncated") == true,
            "A large write's receipt was unbounded or not marked as cut.");
    }
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

    // The worker that may write, in the model's own tool-call format: three tools, none asking for a
    // digest; a call becomes the same action request, a write with its digest left for the host.
    const auto tools = agents::WritingWorkerTools();
    Check(tools.size() == 3 && tools[0].name == "list_directory" && tools[1].name == "read_text_file" && tools[2].name == "write_text_file",
        "The writing worker's tools are not list, read and write.");
    for (const auto& tool : tools)
        Check(tool.parametersSchema.find("digest") == std::string::npos && nlohmann::json::parse(tool.parametersSchema).is_object(),
            "A writing worker's tool asks the model for a digest, or has no valid schema.");
    Check(agents::ParseWritingWorkerCall({"c1", "write_text_file", R"({"path":"C:/workspace/a.py","content":"x = 1\n"})"}, action, error) &&
              action && action->type == actions::ActionType::WriteTextFile && action->value == "x = 1\n" && action->expectedDigest == "missing" &&
              actions::PathToUtf8(action->source) == "C:/workspace/a.py" && action->requestedBy == "agent-workflow",
        "A writing worker's write call did not become a write request waiting for the host's digest.");
    Check(agents::ParseWritingWorkerCall({"c2", "read_text_file", R"({"path":"C:/workspace/a.py"})"}, action, error) && action &&
              action->type == actions::ActionType::ReadTextFile,
        "A writing worker's read call was refused.");
    for (const auto& [name, arguments] : std::vector<std::pair<std::string, std::string>>{{"run_shell_command", R"({"command":"dir"})"},
             {"read_text_file", R"({"path":"relative.txt"})"}, {"read_text_file", R"({"path":"C:/a.txt","extra":1})"},
             {"write_text_file", R"({"path":"C:/a.txt"})"}, {"write_text_file", R"({"path":"C:/a.txt","content":"x","expected_digest":"y"})"},
             {"write_text_file", "{\"path\":\"C:/a.txt\",\"content\":\"" + std::string(agents::MaximumWritingWorkerContentBytes + 1, 'x') + "\"}"},
             {"list_directory", "not json"}})
    {
        Check(!agents::ParseWritingWorkerCall({"bad", name, arguments}, action, error) && !action && error.starts_with("Not "),
            "A malformed writing-worker call was accepted, or refused without telling the model why: " + name + " " + arguments.substr(0, 60));
    }
    Check(!agents::ParseWritingWorkerCall({"bad", "run_shell_command", "{}"}, action, error) && error.find("list_directory") != std::string::npos,
        "An unknown tool was refused without naming the tools there are.");

    // Shown to the model as plain text: a listing without its labels, a file as it is.
    actions::ActionRequest nativeListing;
    nativeListing.type = actions::ActionType::ListDirectory;
    nativeListing.source = actions::Utf8ToPath("C:/workspace");
    actions::ActionOutcome listed;
    listed.result.attempted = listed.result.succeeded = true;
    listed.result.entries = {"[DIR]   sub", "[FILE]  a.py"};
    Check(agents::WritingWorkerToolResult(nativeListing, listed) == "sub/\na.py\n", "A listing was not shown one name a line.");
    actions::ActionRequest reading;
    reading.type = actions::ActionType::ReadTextFile;
    reading.source = actions::Utf8ToPath("C:/workspace/a.py");
    actions::ActionOutcome nativeRead;
    nativeRead.result.attempted = nativeRead.result.succeeded = true;
    nativeRead.result.content = "x = 1\n";
    nativeRead.result.entries = {"sha256:" + audit::ContentDigest("x = 1\n")};
    Check(agents::WritingWorkerToolResult(reading, nativeRead) == "x = 1\n", "A read was not shown as the file's own text, without its digest line.");
    actions::ActionRequest unread;
    unread.type = actions::ActionType::WriteTextFile;
    unread.source = reading.source;
    unread.expectedDigest = "missing";
    actions::ActionOutcome refused;
    refused.result.message = "The file could not be opened exclusively with the expected existence state (Windows error 80).";
    Check(agents::WritingWorkerToolResult(unread, refused).find("Read the file first") != std::string::npos,
        "A write to a file the worker had not read was refused without saying to read it first.");
}
