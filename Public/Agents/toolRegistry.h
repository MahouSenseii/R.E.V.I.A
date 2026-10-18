#pragma once

#include "LLM/toolCall.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::agents
{

// What a tool may do to the world. Read-only tools that are also safe to overlap may run
// together; everything else runs one at a time, in the order the model asked.
enum class ToolEffect
{
    ReadOnly,
    Effect
};

// The turn a tool call belongs to. Executors check it before they act, so a call cannot
// outlive the stop, the audience or the admission of the turn that asked for it.
struct ToolCallContext
{
    std::stop_token stopToken;
    std::function<bool()> admission;
    std::uint64_t turnId = 0;

    [[nodiscard]] bool Admitted() const;
};

// What one call returned. `content` is raw executor output; the tool loop bounds it,
// neutralizes runtime markers and labels it as untrusted before the model sees it.
struct ToolExecution
{
    bool succeeded = false;
    std::string content;
    std::string error;

    [[nodiscard]] static ToolExecution Ok(std::string content);
    [[nodiscard]] static ToolExecution Fail(std::string error);
};

// One tool as the loop runs it: its definition, how to say what it is doing, and the
// executor bound to the owner that already does this work.
struct ChatTool
{
    llm::ToolDefinition definition;
    ToolEffect effect = ToolEffect::ReadOnly;
    // True when two calls may run at the same time. A tool that may ask the person for
    // approval never overlaps, so two prompts cannot appear at once.
    bool parallelSafe = false;
    // "Searching the web for X", from validated arguments.
    std::function<std::string(const nlohmann::json&)> describe;
    std::function<ToolExecution(const nlohmann::json&, const ToolCallContext&)> execute;
};

// The effective permissions for one turn, read from the same settings and audience that
// already decide what Revia may do. The registry never widens any of them.
struct ToolPermissions
{
    // Public, guest and channel audiences get no tools at all.
    bool privateAudience = false;
    // A JSON-shaped reply, a proactive opening or an evaluation turn gets none either.
    bool conversationReply = true;
    bool internetEnabled = false;
    bool automaticLookup = false;
    // The current input explicitly asked to search, which the owner's "automatic lookup
    // off" still allows today.
    bool explicitSearchRequest = false;
    bool memoryEnabled = false;
    bool archiveEnabled = false;
    // Documents the user attached are kept and the main model runs on this computer.
    bool documentsAvailable = false;
    // Folders read_file and list_dir may name; empty means no file tools.
    std::vector<std::filesystem::path> approvedRoots;
    bool remindersAvailable = false;
    bool imageGenerationEnabled = false;
};

// Bound to the existing owners: the internet lookup, memory retrieval, the conversation
// archive, the attachment library, ActionRuntime, the reminder book and the image provider. A missing executor
// means the tool is not offered, whatever the permissions say.
struct ToolExecutors
{
    std::function<ToolExecution(const std::string& query, const ToolCallContext&)> webSearch;
    std::function<ToolExecution(const std::string& query, const ToolCallContext&)> searchMemory;
    std::function<ToolExecution(const std::string& topic, const std::string& when, bool earliest, const ToolCallContext&)> recallHistory;
    std::function<ToolExecution(const std::string& query, const ToolCallContext&)> searchDocuments;
    std::function<ToolExecution(const std::string& path, const ToolCallContext&)> readFile;
    std::function<ToolExecution(const std::string& path, const ToolCallContext&)> listDirectory;
    std::function<ToolExecution(const std::string& text, const std::string& when, const ToolCallContext&)> setReminder;
    std::function<ToolExecution(const std::string& prompt, const ToolCallContext&)> generateImage;
};

// The tools one turn may offer the model, projected from that turn's permissions.
class ToolRegistry
{
  public:
    [[nodiscard]] static ToolRegistry Project(const ToolPermissions& permissions, const ToolExecutors& executors);

    [[nodiscard]] bool Empty() const;
    [[nodiscard]] const ChatTool* Find(const std::string& name) const;
    [[nodiscard]] std::vector<llm::ToolDefinition> Definitions() const;
    [[nodiscard]] const std::vector<ChatTool>& Tools() const;
    // For tests and callers composing a registry by hand.
    void Add(ChatTool tool);

  private:
    std::vector<ChatTool> tools;
};

inline constexpr std::size_t MaximumToolArgumentBytes = 8192;

// Parses `arguments` and checks it strictly against the subset of JSON Schema the
// registry writes: an object with typed properties, required keys and no others;
// strings with length bounds, integers and numbers with ranges, booleans, and arrays of
// one item type with a maximum length (written for MCP tools). Duplicate keys, nesting
// past four levels, invalid UTF-8 and anything over MaximumToolArgumentBytes fail.
[[nodiscard]] bool ValidateToolArguments(
    const std::string& schemaJson, const std::string& arguments, nlohmann::json& outArguments, std::string& outError);

} // namespace revia::agents
