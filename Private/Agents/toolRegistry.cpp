#include "Agents/toolRegistry.h"

#include "Actions/actionTypes.h"
#include "Core/utf8.h"
#include "Evaluation/calculationVerifier.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace revia::agents
{
namespace
{
using Json = nlohmann::json;

constexpr int MaximumArgumentDepth = 4;
constexpr std::size_t DescribedValueBytes = 80;

Json StringSchema(const std::size_t minimum, const std::size_t maximum, const std::string& description)
{
    return {{"type", "string"}, {"minLength", minimum}, {"maxLength", maximum}, {"description", description}};
}

std::string ObjectSchema(Json properties, const std::vector<std::string>& required)
{
    return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", required}, {"additionalProperties", false}}.dump();
}

// For the activity line only: short, one line, valid UTF-8.
std::string Shown(const std::string& value)
{
    std::string line = utf8::Sanitize(value);
    std::replace(line.begin(), line.end(), '\n', ' ');
    std::replace(line.begin(), line.end(), '\r', ' ');
    return line.size() > DescribedValueBytes ? utf8::Prefix(line, DescribedValueBytes) + "..." : line;
}

std::string Text(const Json& arguments, const char* key)
{
    return arguments.contains(key) && arguments[key].is_string() ? arguments[key].get<std::string>() : std::string{};
}

std::size_t CodePoints(const std::string& text)
{
    return static_cast<std::size_t>(
        std::count_if(text.begin(), text.end(), [](const unsigned char byte) { return (byte & 0xC0) != 0x80; }));
}

// The approved roots, named in the file tools' descriptions so the model asks for paths
// that can succeed. Bounded: a long list would cost every request its room.
std::string DescribeRoots(const std::vector<std::filesystem::path>& roots)
{
    std::string listed;
    for (std::size_t index = 0; index < roots.size() && index < 6; ++index)
        listed += (index == 0 ? "" : "; ") + actions::PathToUtf8(roots[index]);
    if (roots.size() > 6)
        listed += "; and " + std::to_string(roots.size() - 6) + " more";
    return listed;
}

bool CheckValue(const Json& schema, const Json& value, const std::string& where, std::string& error)
{
    const std::string type = schema.value("type", "");
    if (schema.contains("enum") && schema["enum"].is_array() &&
        std::find(schema["enum"].begin(), schema["enum"].end(), value) == schema["enum"].end())
    {
        error = where + " is not one of the allowed values.";
        return false;
    }
    if (type == "object")
    {
        if (!value.is_object())
        {
            error = where + " must be an object.";
            return false;
        }
        const Json properties = schema.value("properties", Json::object());
        for (const auto& item : value.items())
        {
            if (!properties.contains(item.key()))
            {
                error = "Unknown argument \"" + Shown(item.key()) + "\".";
                return false;
            }
            if (!CheckValue(properties[item.key()], item.value(), "\"" + item.key() + "\"", error))
                return false;
        }
        for (const auto& required : schema.value("required", Json::array()))
        {
            if (!required.is_string() || !value.contains(required.get<std::string>()))
            {
                error = "Missing required argument " + required.dump() + ".";
                return false;
            }
        }
        return true;
    }
    if (type == "string")
    {
        if (!value.is_string())
        {
            error = where + " must be a string.";
            return false;
        }
        const auto& text = value.get_ref<const std::string&>();
        if (text.find('\0') != std::string::npos || !utf8::IsValid(text))
        {
            error = where + " is not valid text.";
            return false;
        }
        const std::size_t length = CodePoints(text);
        if (length < schema.value("minLength", std::size_t{0}) || length > schema.value("maxLength", MaximumToolArgumentBytes))
        {
            error = where + " is outside its allowed length.";
            return false;
        }
        return true;
    }
    if (type == "integer")
    {
        if (!value.is_number_integer())
        {
            error = where + " must be an integer.";
            return false;
        }
        const auto number = value.get<long long>();
        if ((schema.contains("minimum") && number < schema["minimum"].get<long long>()) ||
            (schema.contains("maximum") && number > schema["maximum"].get<long long>()))
        {
            error = where + " is outside its allowed range.";
            return false;
        }
        return true;
    }
    if (type == "boolean")
    {
        if (!value.is_boolean())
        {
            error = where + " must be true or false.";
            return false;
        }
        return true;
    }
    // A schema keyword this validator does not know is a schema it cannot vouch for.
    error = where + " has an unsupported schema type.";
    return false;
}

ChatTool WebSearchTool(const ToolExecutors& executors)
{
    ChatTool tool;
    tool.definition = {"web_search",
        "Search the web through the owner's approved read-only search provider. Use it for current events, recent releases, "
        "prices, or any fact you are not sure of. Returns page excerpts with their source URLs.",
        ObjectSchema({{"query", StringSchema(1, 256, "What to search for, as a short search-engine query.")}}, {"query"})};
    tool.parallelSafe = true;
    tool.describe = [](const Json& arguments) { return "Searching the web for \"" + Shown(Text(arguments, "query")) + "\""; };
    tool.execute = [search = executors.webSearch](const Json& arguments, const ToolCallContext& context)
    { return search(Text(arguments, "query"), context); };
    return tool;
}

ChatTool SearchMemoryTool(const ToolExecutors& executors)
{
    ChatTool tool;
    tool.definition = {"search_memory",
        "Search your saved long-term memories: facts about the person you are talking with and findings you kept.",
        ObjectSchema({{"query", StringSchema(1, 256, "What to look for.")}}, {"query"})};
    tool.parallelSafe = true;
    tool.describe = [](const Json& arguments) { return "Searching memories for \"" + Shown(Text(arguments, "query")) + "\""; };
    tool.execute = [search = executors.searchMemory](const Json& arguments, const ToolCallContext& context)
    { return search(Text(arguments, "query"), context); };
    return tool;
}

ChatTool RecallHistoryTool(const ToolExecutors& executors)
{
    ChatTool tool;
    tool.definition = {"recall_history", "Search the archive of earlier conversations with this person for what was actually said.",
        ObjectSchema({{"topic", StringSchema(0, 200, "The subject to find. May be empty when 'when' names the time.")},
                         {"when", StringSchema(0, 64, "Optional time in plain words, such as 'yesterday' or 'last week'.")},
                         {"earliest", {{"type", "boolean"}, {"description", "True to find when the topic first came up."}}}},
            {"topic"})};
    tool.parallelSafe = true;
    tool.describe = [](const Json& arguments)
    {
        const std::string topic = Text(arguments, "topic");
        const std::string when = Text(arguments, "when");
        return "Searching past conversations" + (topic.empty() ? std::string{} : " for \"" + Shown(topic) + "\"") +
               (when.empty() ? std::string{} : " (" + Shown(when) + ")");
    };
    tool.execute = [recall = executors.recallHistory](const Json& arguments, const ToolCallContext& context)
    {
        const bool earliest = arguments.contains("earliest") && arguments["earliest"].get<bool>();
        return recall(Text(arguments, "topic"), Text(arguments, "when"), earliest, context);
    };
    return tool;
}

ChatTool FileTool(const bool listing, const ToolExecutors& executors, const std::string& roots)
{
    ChatTool tool;
    tool.definition = {listing ? "list_dir" : "read_file",
        listing
            ? "List the files and folders in a folder. Only these approved folders and what is inside them can be listed: " + roots + "."
            : "Read a text file. Only files inside these approved folders can be read: " + roots + ".",
        ObjectSchema({{"path", StringSchema(1, 2048, "The absolute path.")}}, {"path"})};
    // Policy may ask the person before a read; prompts never overlap.
    tool.parallelSafe = false;
    tool.describe = [listing](const Json& arguments)
    {
        const std::filesystem::path path = actions::Utf8ToPath(Text(arguments, "path"));
        const std::string name = path.has_filename() ? actions::PathToUtf8(path.filename()) : actions::PathToUtf8(path);
        return (listing ? "Listing " : "Reading ") + Shown(name);
    };
    auto executor = listing ? executors.listDirectory : executors.readFile;
    tool.execute = [executor](const Json& arguments, const ToolCallContext& context) { return executor(Text(arguments, "path"), context); };
    return tool;
}

ChatTool ReminderTool(const ToolExecutors& executors)
{
    ChatTool tool;
    tool.definition = {"set_reminder", "Set a reminder or timer that Revia announces when it is due.",
        ObjectSchema({{"text", StringSchema(1, 200, "What to remind about, for example 'stretch'.")},
                         {"when", StringSchema(1, 64, "When, in plain words: 'in 10 minutes', 'at 3pm', 'tomorrow at 9am'.")}},
            {"text", "when"})};
    tool.effect = ToolEffect::Effect;
    tool.describe = [](const Json& arguments)
    { return "Setting a reminder: " + Shown(Text(arguments, "text")) + " (" + Shown(Text(arguments, "when")) + ")"; };
    tool.execute = [set = executors.setReminder](const Json& arguments, const ToolCallContext& context)
    { return set(Text(arguments, "text"), Text(arguments, "when"), context); };
    return tool;
}

ChatTool CalculatorTool()
{
    ChatTool tool;
    tool.definition = {"calculator",
        "Evaluate arithmetic exactly instead of estimating it: + - * / ** and parentheses on decimal numbers, with optional units "
        "s, min, h, mm, cm, m, km, g, kg. It checks only the arithmetic you give it.",
        ObjectSchema({{"expression", StringSchema(1, 512, "For example '(12.5 * 4) ** 2 / 3' or '2 h + 30 min'.")},
                         {"unit", StringSchema(0, 8, "Optional unit for the result, such as 'min'.")}},
            {"expression"})};
    tool.parallelSafe = true;
    tool.describe = [](const Json& arguments) { return "Calculating " + Shown(Text(arguments, "expression")); };
    tool.execute = [](const Json& arguments, const ToolCallContext& context)
    {
        Json input = {{"expression", Text(arguments, "expression")}};
        if (const std::string unit = Text(arguments, "unit"); !unit.empty())
            input["unit"] = unit;
        // The same native verifier that grounds arithmetic in ordinary replies.
        const auto result = evaluation::VerifyCalculation(input.dump(), context.stopToken);
        if (!result.succeeded)
            return ToolExecution::Fail("The calculator refused: " + result.refusal);
        return ToolExecution::Ok(Json{{"expression", result.expression}, {"value", result.value}, {"unit", result.unit}}.dump());
    };
    return tool;
}

ChatTool ImageTool(const ToolExecutors& executors)
{
    ChatTool tool;
    tool.definition = {"generate_image",
        "Generate a picture with the local image model and show it on the Canvas tab. Use it only when a picture was asked for.",
        ObjectSchema({{"prompt", StringSchema(1, 600, "A description of the picture.")}}, {"prompt"})};
    tool.effect = ToolEffect::Effect;
    tool.describe = [](const Json& arguments) { return "Generating a picture of " + Shown(Text(arguments, "prompt")); };
    tool.execute = [generate = executors.generateImage](const Json& arguments, const ToolCallContext& context)
    { return generate(Text(arguments, "prompt"), context); };
    return tool;
}
}

bool ToolCallContext::Admitted() const
{
    if (stopToken.stop_requested())
        return false;
    try
    {
        return !admission || admission();
    }
    catch (...)
    {
        return false;
    }
}

ToolExecution ToolExecution::Ok(std::string content)
{
    ToolExecution execution;
    execution.succeeded = true;
    execution.content = std::move(content);
    return execution;
}

ToolExecution ToolExecution::Fail(std::string error)
{
    ToolExecution execution;
    execution.error = std::move(error);
    return execution;
}

ToolRegistry ToolRegistry::Project(const ToolPermissions& permissions, const ToolExecutors& executors)
{
    ToolRegistry registry;
    if (!permissions.privateAudience || !permissions.conversationReply)
        return registry;
    // Automatic lookup off still honours an explicit "search the web for", as the
    // deterministic lookup does; it never lets the model decide to search on its own.
    if (permissions.internetEnabled && (permissions.automaticLookup || permissions.explicitSearchRequest) && executors.webSearch)
        registry.Add(WebSearchTool(executors));
    if (permissions.memoryEnabled && executors.searchMemory)
        registry.Add(SearchMemoryTool(executors));
    if (permissions.archiveEnabled && executors.recallHistory)
        registry.Add(RecallHistoryTool(executors));
    if (!permissions.approvedRoots.empty())
    {
        const std::string roots = DescribeRoots(permissions.approvedRoots);
        if (executors.readFile)
            registry.Add(FileTool(false, executors, roots));
        if (executors.listDirectory)
            registry.Add(FileTool(true, executors, roots));
    }
    if (permissions.remindersAvailable && executors.setReminder)
        registry.Add(ReminderTool(executors));
    registry.Add(CalculatorTool());
    if (permissions.imageGenerationEnabled && executors.generateImage)
        registry.Add(ImageTool(executors));
    AddSandboxTools(registry, permissions.sandbox, executors.sandbox);
    return registry;
}

bool ToolRegistry::Empty() const
{
    return tools.empty();
}

const ChatTool* ToolRegistry::Find(const std::string& name) const
{
    const auto found = std::find_if(tools.begin(), tools.end(), [&name](const ChatTool& tool) { return tool.definition.name == name; });
    return found == tools.end() ? nullptr : &*found;
}

std::vector<llm::ToolDefinition> ToolRegistry::Definitions() const
{
    std::vector<llm::ToolDefinition> definitions;
    definitions.reserve(tools.size());
    for (const auto& tool : tools)
        definitions.push_back(tool.definition);
    return definitions;
}

const std::vector<ChatTool>& ToolRegistry::Tools() const
{
    return tools;
}

void ToolRegistry::Add(ChatTool tool)
{
    tools.push_back(std::move(tool));
}

bool ValidateToolArguments(
    const std::string& schemaJson, const std::string& arguments, Json& outArguments, std::string& outError, const std::size_t maximumBytes)
{
    outArguments = Json::object();
    outError.clear();
    const Json schema = Json::parse(schemaJson, nullptr, false);
    if (!schema.is_object())
    {
        outError = "The tool's own schema is invalid.";
        return false;
    }
    const std::string text = arguments.find_first_not_of(" \t\r\n") == std::string::npos ? std::string("{}") : arguments;
    if (text.size() > maximumBytes || text.find('\0') != std::string::npos || !utf8::IsValid(text))
    {
        outError = "The arguments are too long or not valid text.";
        return false;
    }
    try
    {
        // One key set per open object, so a repeated key is caught where it repeats.
        std::vector<std::set<std::string>> keys;
        outArguments = Json::parse(text,
            [&keys](const int depth, const Json::parse_event_t event, Json& parsed)
            {
                if (depth > MaximumArgumentDepth)
                    throw std::runtime_error("The arguments are nested too deeply.");
                if (event == Json::parse_event_t::object_start)
                    keys.emplace_back();
                else if (event == Json::parse_event_t::object_end && !keys.empty())
                    keys.pop_back();
                else if (event == Json::parse_event_t::key && (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second))
                    throw std::runtime_error("The arguments repeat a key.");
                return true;
            });
    }
    catch (const std::runtime_error& error)
    {
        outError = error.what();
        outArguments = Json::object();
        return false;
    }
    catch (const Json::exception&)
    {
        outError = "The arguments are not valid JSON.";
        outArguments = Json::object();
        return false;
    }
    if (!CheckValue(schema, outArguments, "The arguments", outError))
    {
        outArguments = Json::object();
        return false;
    }
    return true;
}

} // namespace revia::agents
