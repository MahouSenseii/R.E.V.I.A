#include "Mcp/mcpSettings.h"

#include "Actions/actionTypes.h"
#include "Core/utf8.h"
#include "LLM/Cloud/cloudTransport.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace
{
bool ServerId(const std::string& id)
{
    return !id.empty() && id.size() <= 24 && (std::islower(static_cast<unsigned char>(id[0])) || std::isdigit(static_cast<unsigned char>(id[0]))) &&
           std::all_of(id.begin(), id.end(), [](const unsigned char c) { return std::islower(c) || std::isdigit(c) || c == '-' || c == '_'; });
}

bool EnvironmentName(const std::string& name)
{
    return !name.empty() && name.size() <= 64 && !std::isdigit(static_cast<unsigned char>(name[0])) &&
           std::all_of(name.begin(), name.end(), [](const unsigned char c) { return std::isalnum(c) || c == '_'; });
}

bool ToolName(const std::string& name)
{
    return !name.empty() && name.size() <= 64 &&
           std::all_of(name.begin(), name.end(), [](const unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; });
}

std::string Lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
} // namespace

std::string ValidateMcpServerSettings(const mcpServerSettings& server)
{
    if (!ServerId(server.id))
        return "An MCP server id must be 1-24 lowercase letters, digits, '-' or '_'.";
    if (server.timeoutSeconds < 1 || server.timeoutSeconds > 600)
        return "MCP server \"" + server.id + "\" needs timeoutSeconds between 1 and 600.";
    if (server.allowedTools.size() > 64 || !std::all_of(server.allowedTools.begin(), server.allowedTools.end(), ToolName))
        return "MCP server \"" + server.id + "\" lists an invalid tool name, or more than 64.";
    if (server.envAllowlist.size() > 32 || !std::all_of(server.envAllowlist.begin(), server.envAllowlist.end(), EnvironmentName))
        return "MCP server \"" + server.id + "\" lists an invalid environment variable name, or more than 32.";
    if (server.transport == "stdio")
    {
        if (!server.url.empty())
            return "MCP server \"" + server.id + "\" uses stdio, so it takes a command, not a url.";
        const std::filesystem::path command = revia::actions::Utf8ToPath(server.command);
        if (server.command.empty() || !revia::utf8::IsValid(server.command) || !command.is_absolute())
            return "MCP server \"" + server.id + "\" needs an absolute command path.";
        std::string name = Lowered(revia::actions::PathToUtf8(command.filename()));
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".exe") != 0)
            name += ".exe";
        // A shell would parse the arguments as a command line. On Windows a .cmd or .bat
        // runs through cmd.exe even when started directly, so only programs are accepted.
        // Python is the exception to the command-surface list: many servers are Python
        // programs, and here the owner, not the model, writes the whole command line.
        if (revia::actions::IsCommandSurfaceExecutable(name) && name != "python.exe" && name != "pythonw.exe")
            return "MCP server \"" + server.id + "\" may not be started through a shell or command interpreter.";
#ifdef _WIN32
        if (command.extension() != L".exe" && command.extension() != L".EXE")
            return "MCP server \"" + server.id + "\" must name a program (.exe); scripts would run through a shell.";
#endif
        if (server.args.size() > 64)
            return "MCP server \"" + server.id + "\" has more than 64 arguments.";
        std::size_t bytes = 0;
        for (const auto& argument : server.args)
        {
            bytes += argument.size();
            if (argument.find('\0') != std::string::npos || !revia::utf8::IsValid(argument) || bytes > 24576)
                return "MCP server \"" + server.id + "\" has an argument that is too long or not valid text.";
        }
        return {};
    }
    if (server.transport == "http")
    {
        if (!server.command.empty() || !server.args.empty() || !server.envAllowlist.empty())
            return "MCP server \"" + server.id + "\" uses http, so it takes a url, not a command.";
        std::string error;
        // The same parser as the cloud backend: HTTPS, or plain http to 127.0.0.1 only.
        if (!revia::llm::cloud::ParseCloudEndpoint(server.url, error))
            return "MCP server \"" + server.id + "\" needs an https:// url, or http://127.0.0.1 for a server on this PC.";
        return {};
    }
    return "MCP server \"" + server.id + "\" needs transport \"stdio\" or \"http\".";
}
