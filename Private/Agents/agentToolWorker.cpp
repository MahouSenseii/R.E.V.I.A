#include "Agents/agentToolWorker.h"

#include "Audit/contentDigest.h"
#include "Core/utf8.h"
#include "Planning/structuredActionParser.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <set>

namespace revia::agents
{
namespace
{
using Json = nlohmann::json;

Json ObjectSchema(Json properties)
{
    Json required = Json::array();
    for (const auto& item : properties.items())
        required.push_back(item.key());
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}};
}

bool Digest(const std::string& text)
{
    return text == "missing" || (text.size() == 64 && std::all_of(text.begin(), text.end(), [](unsigned char ch)
                                                          { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }));
}

std::string BoundedText(const std::string& text, std::size_t maximum)
{
    return utf8::Prefix(utf8::Sanitize(text.substr(0, maximum)), maximum);
}

void AddPathIdentity(Json& receipt, const char* field, const std::filesystem::path& path)
{
    if (path.empty())
        return;
    const auto identity = actions::PathToUtf8(path);
    receipt["pathDigests"][field] = audit::ContentDigest(identity);
    receipt[field] = utf8::IsValid(identity) ? Json(identity) : Json(nullptr);
    if (receipt[field].is_null())
        receipt["truncated"] = true;
}
}

std::string WorkerToolSchema(const bool allowWrites, const bool allowProcess, const bool mayEnd)
{
    const Json path = {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048}};
    Json choices = Json::array();
    if (mayEnd)
        choices.push_back(Json{{"type", "null"}});
    choices.push_back(ObjectSchema({{"action", {{"enum", {"list_directory", "read_text_file"}}}}, {"source", path}}));
    if (allowWrites)
        choices.push_back(ObjectSchema({{"action", {{"const", "write_text_file"}}}, {"source", path},
            {"content", {{"type", "string"}, {"maxLength", 8192}}}, {"expected_digest", {{"type", "string"}, {"maxLength", 64}}}}));
    if (allowProcess)
        choices.push_back(ObjectSchema({{"action", {{"const", "execute_process"}}}, {"executable", path}, {"working_directory", path},
            {"arguments", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}, {"maxLength", 1024}}}}},
            {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}}}));
    return ObjectSchema({{"tool", {{"anyOf", std::move(choices)}}}}).dump();
}

bool ParseWorkerToolResponse(const std::string& response, const bool allowWrites, const bool allowProcess,
    std::optional<actions::ActionRequest>& outAction, std::string& outError)
{
    outAction.reset();
    outError = "The worker returned an invalid or unavailable bounded tool request.";
    if (response.empty() || response.size() > 16384 || !utf8::IsValid(response) || response.find('\0') != std::string::npos)
        return false;
    try
    {
        const auto envelope = Json::parse(response);
        if (!envelope.is_object() || envelope.size() != 1 || !envelope.contains("tool"))
            return false;
        const auto& tool = envelope.at("tool");
        if (tool.is_null())
        {
            outError.clear();
            return true;
        }
        if (!tool.is_object())
            return false;
        const auto type = actions::ActionTypeFromString(tool.at("action").get<std::string>());
        std::set<std::string> fields{"action", "source"};
        if (type == actions::ActionType::WriteTextFile && allowWrites)
            fields.insert({"content", "expected_digest"});
        else if (type == actions::ActionType::ExecuteProcess && allowProcess)
            fields = {"action", "executable", "working_directory", "arguments", "timeout_ms"};
        else if (type != actions::ActionType::ReadTextFile && type != actions::ActionType::ListDirectory)
            return false;
        if (tool.size() != fields.size())
            return false;
        for (const auto& field : tool.items())
            if (!fields.contains(field.key()))
                return false;
        auto parsed = planning::StructuredActionParser::ParseObject(tool);
        if (!parsed.succeeded)
            return false;
        auto& request = parsed.request;
        const auto validPath = [](const std::filesystem::path& path)
        {
            const auto value = actions::PathToUtf8(path);
            return path.is_absolute() && value.size() <= 2048 && value.find('\0') == std::string::npos && utf8::IsValid(value);
        };
        if (type == actions::ActionType::ExecuteProcess)
        {
            if (!validPath(request.process.executable) || !validPath(request.process.workingDirectory) ||
                request.process.arguments.size() > 32 || request.process.timeoutMs < 1 || request.process.timeoutMs > 10000)
                return false;
            for (const auto& argument : request.process.arguments)
                if (argument.size() > 1024 || argument.find('\0') != std::string::npos || !utf8::IsValid(argument))
                    return false;
            // The program is named once, by `executable`. A live 9B also repeated it as the first
            // argument in 13 of 13 replayed runs, as a command line would, so Python was handed its
            // own executable as the script to run and the test never started. Naming the program
            // again is never a real argument to it, so the repeat is dropped.
            if (!request.process.arguments.empty())
            {
                const auto same = [](std::string a, std::string b)
                {
                    const auto fold = [](std::string& text)
                    {
                        std::replace(text.begin(), text.end(), '\\', '/');
                        std::transform(text.begin(), text.end(), text.begin(),
                            [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    };
                    fold(a);
                    fold(b);
                    return a == b;
                };
                const auto& first = request.process.arguments.front();
                if (same(first, actions::PathToUtf8(request.process.executable)) ||
                    same(first, actions::PathToUtf8(request.process.executable.filename())))
                    request.process.arguments.erase(request.process.arguments.begin());
            }
        }
        else if (!validPath(request.source))
            return false;
        if (type == actions::ActionType::WriteTextFile &&
            (!Digest(request.expectedDigest) || request.value.size() > 8192 || request.value.find('\0') != std::string::npos ||
                !utf8::IsValid(request.value)))
            return false;
        if (type == actions::ActionType::WriteTextFile && request.expectedDigest.size() == 64)
        {
            // A read's receipt ends its output with the file's digest line. Rewriting a file from
            // that output, a live 9B copied the line into the new content in 8 of 19 replayed
            // writes, and the file no longer parsed. The line is the host's, never the file's: it
            // is the digest of the very content being replaced, which the worker has just quoted.
            const std::string echoed = "sha256:" + request.expectedDigest;
            auto& content = request.value;
            std::size_t end = content.size();
            while (end > 0 && (content[end - 1] == '\n' || content[end - 1] == '\r'))
                --end;
            if (end >= echoed.size() && content.compare(end - echoed.size(), echoed.size(), echoed) == 0 &&
                (end == echoed.size() || content[end - echoed.size() - 1] == '\n'))
                content.erase(end - echoed.size());
        }
        request.requestedBy = "agent-workflow";
        outAction = std::move(request);
        outError.clear();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::vector<llm::ToolDefinition> WritingWorkerTools()
{
    const auto pathOnly = [](const char* about)
    {
        return Json{{"type", "object"}, {"properties", {{"path", {{"type", "string"}, {"description", about}}}}},
            {"required", Json::array({"path"})}, {"additionalProperties", false}}
            .dump();
    };
    return {
        {"list_directory", "List the files and folders in one folder.", pathOnly("Absolute path of a folder inside the approved folders.")},
        {"read_text_file", "Read one text file.", pathOnly("Absolute path of a file inside the approved folders.")},
        {"write_text_file", "Replace a text file you have read with new content, or create a new file. Write the whole file.",
            Json{{"type", "object"},
                {"properties", {{"path", {{"type", "string"}, {"description", "Absolute path of the file inside the approved folders."}}},
                                   {"content", {{"type", "string"}, {"description", "The whole new text of the file."}}}}},
                {"required", Json::array({"path", "content"})}, {"additionalProperties", false}}
                .dump()}};
}

bool ParseWritingWorkerCall(const llm::ToolCall& call, std::optional<actions::ActionRequest>& outAction, std::string& outError)
{
    outAction.reset();
    const bool write = call.name == "write_text_file";
    if (!write && call.name != "read_text_file" && call.name != "list_directory")
    {
        outError = "Not run: there is no tool named \"" + utf8::Prefix(utf8::Sanitize(call.name), 64) +
                   "\". The tools are list_directory, read_text_file and write_text_file.";
        return false;
    }
    outError = write ? "Not run: write_text_file takes a JSON object with an absolute path and the whole new content."
                     : "Not run: " + call.name + " takes a JSON object with one absolute path.";
    if (call.arguments.size() > 4 * MaximumWritingWorkerContentBytes || !utf8::IsValid(call.arguments))
        return false;
    try
    {
        const auto arguments = Json::parse(call.arguments);
        if (!arguments.is_object() || arguments.size() != (write ? 2U : 1U) || !arguments.contains("path") || !arguments["path"].is_string() ||
            (write && (!arguments.contains("content") || !arguments["content"].is_string())))
            return false;
        const auto& path = arguments["path"].get_ref<const std::string&>();
        if (path.empty() || path.size() > 2048 || path.find('\0') != std::string::npos || !actions::Utf8ToPath(path).is_absolute())
        {
            outError = "Not run: the path must be absolute, inside the approved folders.";
            return false;
        }
        Json action = {{"action", call.name}, {"source", path}};
        if (write)
        {
            const auto& content = arguments["content"].get_ref<const std::string&>();
            if (content.size() > MaximumWritingWorkerContentBytes || content.find('\0') != std::string::npos)
            {
                outError = "Not written: the content is longer than " + std::to_string(MaximumWritingWorkerContentBytes) +
                           " bytes, more than one write may hold.";
                return false;
            }
            action["content"] = content;
            action["expected_digest"] = "missing";
        }
        auto parsed = planning::StructuredActionParser::ParseObject(action);
        if (!parsed.succeeded)
            return false;
        parsed.request.requestedBy = "agent-workflow";
        outAction = std::move(parsed.request);
        outError.clear();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::string WritingWorkerToolResult(const actions::ActionRequest& request, const actions::ActionOutcome& outcome)
{
    const std::string name = utf8::Sanitize(actions::PathToUtf8(request.source.filename()));
    if (!outcome.Succeeded())
    {
        std::string said = BoundedText(outcome.Message(), 1024);
        // The host checks a write against this worker's own read of that file; with no read
        // there is nothing to check it against, and the model never handles a digest itself.
        if (request.type == actions::ActionType::WriteTextFile && request.expectedDigest == "missing")
            said += " Read the file first: a write replaces a file this worker has read, and the host checks it against that read.";
        return said.empty() ? "The host could not run this call." : said;
    }
    if (request.type == actions::ActionType::ListDirectory)
    {
        std::string listing;
        for (const auto& entry : outcome.result.entries)
        {
            if (listing.size() >= MaximumWritingWorkerContentBytes)
            {
                listing += "[more entries not shown]\n";
                break;
            }
            const bool folder = entry.starts_with("[DIR]");
            const auto first = entry.starts_with("[") ? entry.find_first_not_of(" \t", entry.find(']') + 1) : 0;
            if (first == std::string::npos)
                continue;
            listing += BoundedText(entry.substr(first), 1024) + (folder ? "/" : "") + "\n";
        }
        return listing.empty() ? "The folder is empty." : listing;
    }
    if (request.type == actions::ActionType::ReadTextFile)
    {
        // The file as it is: no digest line, which a model rewriting the file copied into it.
        std::string text = BoundedText(outcome.result.content, MaximumWritingWorkerContentBytes);
        if (text.size() < outcome.result.content.size())
            text += "\n[The file is longer than " + std::to_string(MaximumWritingWorkerContentBytes) + " bytes; the rest is not shown.]";
        return text.empty() ? "The file is empty." : text;
    }
    return "Wrote " + std::to_string(request.value.size()) + " bytes to " + name + ".";
}

std::string WorkerToolReceipt(const actions::ActionRequest& request, const actions::ActionOutcome& outcome)
{
    std::string output = BoundedText(outcome.result.content, 8192);
    bool truncated = output.size() < outcome.result.content.size();
    for (const auto& entry : outcome.result.entries)
    {
        if (output.size() >= 8192)
        {
            truncated = true;
            break;
        }
        output += BoundedText(entry, 8192 - output.size()) + "\n";
    }
    const Json contentDigest =
        request.type == actions::ActionType::ListDirectory ? Json(nullptr) : Json(audit::ContentDigest(outcome.result.content));
    Json receipt = {{"actionId", BoundedText(request.id, 128)}, {"action", actions::ToString(request.type)},
        {"succeeded", outcome.Succeeded()}, {"attempted", outcome.result.attempted}, {"message", BoundedText(outcome.Message(), 1024)},
        {"contentDigest", contentDigest}, {"output", output}, {"truncated", truncated}};
    std::vector<std::string> arguments;
    if (request.type == actions::ActionType::ExecuteProcess)
    {
        AddPathIdentity(receipt, "executable", request.process.executable);
        AddPathIdentity(receipt, "working_directory", request.process.workingDirectory);
        // What was run. A receipt gave the program, its folder and its output, never the
        // arguments, so a write-up made from receipts could say "exit code 0" and not which
        // script that was, and a second run by the same worker looked like the first.
        std::size_t bytes = 0;
        for (const auto& argument : request.process.arguments)
        {
            auto shown = BoundedText(argument, 1024);
            bytes += shown.size();
            if (shown.size() < argument.size() || bytes > WorkerToolArgumentBytes)
            {
                receipt["truncated"] = true;
                break;
            }
            arguments.push_back(std::move(shown));
        }
        receipt["arguments"] = arguments;
    }
    else
        AddPathIdentity(receipt, "source", request.source);
    if (outcome.result.process)
    {
        const auto& process = *outcome.result.process;
        output = BoundedText(process.standardOutput, 4096) + "\n" + BoundedText(process.standardError, 4096);
        receipt["output"] = output;
        receipt["truncated"] = process.outputTruncated || output.size() < process.standardOutput.size() + process.standardError.size();
        receipt["exitCode"] = process.exitCode;
        receipt["timedOut"] = process.timedOut;
        receipt["cancelled"] = process.cancelled;
    }
    // What the host wrote. The receipt otherwise carried only the new file's hash, and a
    // worker asked whether its change was established could not tell from a hash: a live 9B
    // fixed the bug it was given, then reported the fix as unverified because no receipt
    // showed the new content, and the run paused before its test was ever run.
    std::string written;
    if (request.type == actions::ActionType::WriteTextFile && outcome.Succeeded())
    {
        written = BoundedText(request.value, 8192);
        receipt["written"] = written;
        if (written.size() < request.value.size())
            receipt["truncated"] = true;
    }
    auto encoded = receipt.dump();
    while (encoded.size() > WorkerToolOutputReservation && !output.empty())
    {
        output = utf8::Prefix(output, output.size() / 2);
        receipt["output"] = output;
        receipt["truncated"] = true;
        encoded = receipt.dump();
    }
    auto message = receipt.at("message").get<std::string>();
    while (encoded.size() > WorkerToolOutputReservation && !message.empty())
    {
        message = utf8::Prefix(message, message.size() / 2);
        receipt["message"] = message;
        receipt["truncated"] = true;
        encoded = receipt.dump();
    }
    while (encoded.size() > WorkerToolOutputReservation && !written.empty())
    {
        written = utf8::Prefix(written, written.size() / 2);
        receipt["written"] = written;
        receipt["truncated"] = true;
        encoded = receipt.dump();
    }
    while (encoded.size() > WorkerToolOutputReservation && !arguments.empty())
    {
        arguments.pop_back();
        receipt["arguments"] = arguments;
        receipt["truncated"] = true;
        encoded = receipt.dump();
    }
    // Identity text is complete or unavailable; its full digest always remains.
    for (const auto* field : {"working_directory", "executable", "source"})
    {
        if (encoded.size() <= WorkerToolOutputReservation)
            break;
        if (receipt.contains(field) && !receipt[field].is_null())
        {
            receipt[field] = nullptr;
            receipt["truncated"] = true;
            encoded = receipt.dump();
        }
    }
    return encoded;
}

} // namespace revia::agents
