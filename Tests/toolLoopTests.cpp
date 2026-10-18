#include "Agents/chatTools.h"
#include "Agents/toolRegistry.h"
#include "Memory/conversationRecall.h"
#include "Planning/reminders.h"
#include "testSupport.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <vector>

namespace
{
using namespace revia::agents;
using revia::tests::Check;
using Json = nlohmann::json;

ToolPermissions EveryPermission()
{
    ToolPermissions permissions;
    permissions.privateAudience = true;
    permissions.internetEnabled = true;
    permissions.automaticLookup = true;
    permissions.memoryEnabled = true;
    permissions.archiveEnabled = true;
    permissions.approvedRoots = {std::filesystem::temp_directory_path()};
    permissions.remindersAvailable = true;
    permissions.imageGenerationEnabled = true;
    return permissions;
}

ToolExecutors EveryExecutor()
{
    ToolExecutors executors;
    const auto ok = [](const std::string& value, const ToolCallContext&) { return ToolExecution::Ok(value); };
    executors.webSearch = ok;
    executors.searchMemory = ok;
    executors.recallHistory = [](const std::string& topic, const std::string&, bool, const ToolCallContext&)
    { return ToolExecution::Ok(topic); };
    executors.readFile = ok;
    executors.listDirectory = ok;
    executors.setReminder = [](const std::string& text, const std::string&, const ToolCallContext&) { return ToolExecution::Ok(text); };
    executors.generateImage = ok;
    return executors;
}

std::vector<std::string> Names(const ToolRegistry& registry)
{
    std::vector<std::string> names;
    for (const auto& tool : registry.Tools())
        names.push_back(tool.definition.name);
    return names;
}

bool Has(const ToolRegistry& registry, const std::string& name)
{
    return registry.Find(name) != nullptr;
}

// A stand-in for the session's action boundary that records what reached the runtime.
struct ApprovalProbe
{
    int executions = 0;
    bool lastConfirmed = false;

    ChatActionPort Port(const revia::actions::PolicyVerdict verdict)
    {
        ChatActionPort port;
        port.evaluate = [verdict](const revia::actions::ActionRequest&)
        {
            revia::actions::PolicyDecision decision;
            decision.verdict = verdict;
            decision.reason = "Reading this file requires confirmation.";
            return decision;
        };
        port.execute = [this, verdict](const revia::actions::ActionRequest& request, const bool confirmed)
        {
            ++executions;
            lastConfirmed = confirmed;
            Check(request.requestedBy == "conversation_tool" && static_cast<bool>(request.beforeEffect),
                "A chat action lost its requester or its turn admission.");
            revia::actions::ActionOutcome outcome;
            outcome.policy.verdict = verdict;
            outcome.result.attempted = outcome.result.succeeded = verdict != revia::actions::PolicyVerdict::Blocked;
            outcome.result.content = outcome.result.succeeded ? "secret plans" : "";
            outcome.result.message = outcome.result.succeeded ? "Read." : "Outside the approved roots.";
            return outcome;
        };
        return port;
    }
};

void TestRegistryProjectsTheTurnsEffectivePermissions()
{
    const auto all = ToolRegistry::Project(EveryPermission(), EveryExecutor());
    Check(Names(all) == std::vector<std::string>{"web_search", "search_memory", "recall_history", "read_file", "list_dir", "set_reminder",
                            "calculator", "generate_image"},
        "A private turn with every permission did not offer every first tool: " + Json(Names(all)).dump());
    for (const auto& definition : all.Definitions())
    {
        const auto schema = Json::parse(definition.parametersSchema);
        Check(schema.at("type") == "object" && schema.at("additionalProperties") == false && !definition.description.empty(),
            "Tool " + definition.name + " has an open or undescribed schema.");
    }

    auto permissions = EveryPermission();
    permissions.privateAudience = false;
    Check(ToolRegistry::Project(permissions, EveryExecutor()).Empty(), "A public audience was offered tools.");
    permissions = EveryPermission();
    permissions.conversationReply = false;
    Check(ToolRegistry::Project(permissions, EveryExecutor()).Empty(), "A JSON-shaped or proactive reply was offered tools.");

    permissions = EveryPermission();
    permissions.internetEnabled = false;
    Check(!Has(ToolRegistry::Project(permissions, EveryExecutor()), "web_search"), "Disabled internet still offered web_search.");
    permissions = EveryPermission();
    permissions.automaticLookup = false;
    Check(!Has(ToolRegistry::Project(permissions, EveryExecutor()), "web_search"),
        "Automatic lookup off still let the model decide to search on its own.");
    permissions.explicitSearchRequest = true;
    Check(Has(ToolRegistry::Project(permissions, EveryExecutor()), "web_search"),
        "An explicit search request was refused web_search although automatic lookup off allows it.");

    permissions = EveryPermission();
    permissions.memoryEnabled = permissions.archiveEnabled = permissions.remindersAvailable = permissions.imageGenerationEnabled = false;
    permissions.approvedRoots.clear();
    const auto narrow = ToolRegistry::Project(permissions, EveryExecutor());
    Check(Names(narrow) == std::vector<std::string>{"web_search", "calculator"},
        "Withdrawn memory, archive, roots, reminders or images still produced tools: " + Json(Names(narrow)).dump());

    auto executors = EveryExecutor();
    executors.readFile = nullptr;
    executors.webSearch = nullptr;
    const auto unbound = ToolRegistry::Project(EveryPermission(), executors);
    Check(!Has(unbound, "read_file") && Has(unbound, "list_dir") && !Has(unbound, "web_search"),
        "A tool with no executor bound to its owner was offered.");
    Check(all.Find("read_file")->definition.description.find(revia::actions::PathToUtf8(std::filesystem::temp_directory_path())) !=
              std::string::npos,
        "The file tools do not name the approved roots they are limited to.");
}

void TestArgumentsAreValidatedStrictly()
{
    const auto registry = ToolRegistry::Project(EveryPermission(), EveryExecutor());
    const auto& search = registry.Find("web_search")->definition.parametersSchema;
    const auto& recall = registry.Find("recall_history")->definition.parametersSchema;
    Json parsed;
    std::string error;
    Check(ValidateToolArguments(search, R"({"query":"llama.cpp tool calling"})", parsed, error) &&
              parsed.at("query") == "llama.cpp tool calling",
        "Valid arguments were refused: " + error);
    Check(ValidateToolArguments(recall, R"({"topic":"","when":"yesterday","earliest":false})", parsed, error),
        "Optional arguments were refused: " + error);
    const std::vector<std::pair<std::string, std::string>> refused = {{R"({"query":"x","host":"evil.example"})", "an unknown key"},
        {R"({})", "a missing required key"}, {"", "empty arguments for a required key"}, {R"({"query":42})", "a number for a string"},
        {R"({"query":""})", "an empty string under minLength"},
        {"{\"query\":\"" + std::string(257, 'a') + "\"}", "a string over maxLength"}, {R"({"query":"a","query":"b"})", "a duplicated key"},
        {R"({"query": )", "truncated JSON"}, {R"(["query"])", "an array instead of an object"},
        {R"({"query":{"a":{"b":{"c":{"d":{"e":1}}}}}})", "nesting past the depth bound"}, {"{\"query\":\"\xff\xfe\"}", "invalid UTF-8"},
        {"{\"query\":\"" + std::string(9000, 'a') + "\"}", "arguments over the byte bound"}};
    for (const auto& [arguments, what] : refused)
        Check(!ValidateToolArguments(search, arguments, parsed, error) && !error.empty() && parsed.empty(),
            "Validation accepted " + what + ".");
    Check(
        !ValidateToolArguments(recall, R"({"topic":"x","earliest":"yes"})", parsed, error), "Validation accepted a string for a boolean.");
}

// A chat file tool takes the same steps an interactive action does: policy, then the
// owner's approval when policy asks, then the runtime. Nothing that needs approval
// reaches the runtime without it.
void TestChatActionsKeepTheApprovalStep()
{
    ApprovalProbe probe;
    ChatActionPort port = probe.Port(revia::actions::PolicyVerdict::RequiresConfirmation);
    const std::string path = revia::actions::PathToUtf8(std::filesystem::temp_directory_path() / "plans.txt");
    const auto read = [&](const ToolCallContext& context)
    { return RunChatAction(port, ChatFileRequest(revia::actions::ActionType::ReadTextFile, path), context); };
    const ToolCallContext context;
    const auto unasked = read(context);
    Check(!unasked.succeeded && unasked.error.find("needs your approval") != std::string::npos && probe.executions == 0,
        "An action that needs approval reached the runtime with no one to ask.");
    port.confirm = [](const auto&, const auto&) { return revia::actions::ConfirmationChoice::Decline; };
    Check(!read(context).succeeded && probe.executions == 0, "A declined action reached the runtime.");
    port.confirm = [](const auto&, const auto&) { return revia::actions::ConfirmationChoice::Allow; };
    const auto approved = read(context);
    Check(approved.succeeded && approved.content == "secret plans" && probe.executions == 1 && probe.lastConfirmed,
        "An approved action did not run with its confirmation.");
    std::stop_source stopped;
    stopped.request_stop();
    Check(!read({stopped.get_token(), {}, 0}).succeeded && probe.executions == 1, "A stopped turn's action ran.");
    Check(!read({{}, [] { return false; }, 0}).succeeded && probe.executions == 1, "A turn that lost admission ran an action.");

    // A blocked action still goes to the runtime, which refuses it before any executor and
    // records the refusal; the tool reports the refusal.
    ApprovalProbe blockedProbe;
    const ChatActionPort blocked = blockedProbe.Port(revia::actions::PolicyVerdict::Blocked);
    const auto refused = RunChatAction(blocked, ChatFileRequest(revia::actions::ActionType::ReadTextFile, path), context);
    Check(!refused.succeeded && refused.error.find("Outside the approved roots") != std::string::npos && blockedProbe.executions == 1 &&
              !blockedProbe.lastConfirmed,
        "A blocked chat action was not handed to the runtime for its audited refusal.");
}

// Reminders and history recall reach their owners through typed requests: the reminder
// parser reads the time, and the archive is searched by extracted terms, not model text.
void TestRemindersAndRecallUseTheirOwners()
{
    revia::planning::ReminderBook book;
    const auto now = revia::planning::WallClock::now();
    const auto set = AddChatReminder(book, "stretch my legs", "in 10 minutes", now);
    const auto pending = book.Pending();
    Check(set.succeeded && pending.size() == 1 && pending[0].request.text == "stretch your legs" &&
              pending[0].request.due - now == std::chrono::minutes(10),
        "A reminder from a tool call did not go through the reminder parser and book: " + set.error);
    Check(!AddChatReminder(book, "stretch", "whenever you like", now).succeeded && book.Pending().size() == 1,
        "A reminder with an unreadable time was set.");

    const auto epoch = revia::memory::CurrentEpoch();
    const auto topic = revia::memory::BuildRecallRequest("the emotion system", "", false, epoch);
    Check(topic.kind == revia::memory::RecallKind::Topic && topic.terms == std::vector<std::string>{"emotion", "system"},
        "A recall topic was not reduced to its subject terms.");
    const auto window = revia::memory::BuildRecallRequest("", "yesterday", false, epoch);
    Check(window.kind == revia::memory::RecallKind::Window && window.window.IsValid(), "A recall time was not parsed into a window.");
    Check(revia::memory::BuildRecallRequest("Project Hunter", "", true, epoch).kind == revia::memory::RecallKind::Earliest,
        "An earliest-mention recall was not typed as one.");
    Check(!revia::memory::BuildRecallRequest("", "", false, epoch).Wanted() &&
              !revia::memory::BuildRecallRequest("what did we", "", false, epoch).Wanted(),
        "A recall with nothing to look for was still sent to the archive.");
}
}

void RunToolLoopTests()
{
    TestRegistryProjectsTheTurnsEffectivePermissions();
    TestArgumentsAreValidatedStrictly();
    TestChatActionsKeepTheApprovalStep();
    TestRemindersAndRecallUseTheirOwners();
}
