#include "Agents/chatTools.h"

#include "Agents/delegation.h"
#include "Core/utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace revia::agents
{

namespace
{
// Slashes one way, ASCII lowered and no trailing separator, so two spellings of one place
// compare equal.
std::string Folded(const std::string_view path)
{
    std::string folded;
    folded.reserve(path.size());
    for (const char character : path)
        folded.push_back(character == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    while (folded.size() > 3 && folded.back() == '/')
        folded.pop_back();
    return folded;
}

// By spelling alone. Policy resolves the real path and decides; this only says whether a
// read is worth asking for.
bool InsideAny(const std::string& folded, const std::vector<std::filesystem::path>& roots)
{
    return std::any_of(roots.begin(), roots.end(), [&folded](const std::filesystem::path& root)
    {
        const std::string base = Folded(actions::PathToUtf8(root));
        return !base.empty() && folded.starts_with(base) &&
               (folded.size() == base.size() || base.back() == '/' || folded[base.size()] == '/');
    });
}

using Span = std::pair<std::size_t, std::size_t>;

// Where each absolute Windows path written in `text` begins and ends.
std::vector<Span> AuthoredPathSpans(const std::string_view text)
{
    std::vector<Span> spans;
    for (std::size_t at = 0; at + 2 < text.size(); ++at)
    {
        if (std::isalpha(static_cast<unsigned char>(text[at])) == 0 || text[at + 1] != ':' || (text[at + 2] != '/' && text[at + 2] != '\\'))
            continue;
        // A drive letter stands alone: the "p:/" inside "http://" is not one.
        if (at > 0 && std::isalnum(static_cast<unsigned char>(text[at - 1])) != 0)
            continue;
        const char quote = at > 0 && (text[at - 1] == '"' || text[at - 1] == '\'' || text[at - 1] == '`') ? text[at - 1] : '\0';
        std::size_t end = at + 3;
        while (end < text.size() && std::string_view("\r\n<>|*?").find(text[end]) == std::string_view::npos &&
               (quote != '\0' ? text[end] != quote : std::string_view(" \t\"'`").find(text[end]) == std::string_view::npos))
            ++end;
        // The sentence's own punctuation is not part of the path.
        while (end > at + 3 && std::string_view(".,;:!)").find(text[end - 1]) != std::string_view::npos)
            --end;
        if (end > at + 3)
            spans.emplace_back(at, end);
        at = end;
    }
    return spans;
}

// File names written in `text` outside any full path: todo.md, or "shopping list.txt" in
// quotes. Only the shape is judged here: a stem, a dot, a short extension. Most words of
// that shape ("e.g", "example.com") name no file, and are dropped when none is found.
std::vector<std::string> AuthoredFileNames(const std::string_view text, const std::vector<Span>& paths)
{
    std::string plain(text);
    for (const auto& [begin, end] : paths)
        std::fill(plain.begin() + static_cast<std::ptrdiff_t>(begin), plain.begin() + static_cast<std::ptrdiff_t>(end), ' ');
    // A pasted page can hold hundreds of dotted words; each kept one costs a look on disk.
    constexpr std::size_t MaximumNames = 16;
    std::vector<std::string> names;
    const auto add = [&names](std::string_view candidate)
    {
        while (!candidate.empty() && candidate.back() == '.')
            candidate.remove_suffix(1);
        const std::size_t dot = candidate.rfind('.');
        if (names.size() >= MaximumNames || dot == std::string_view::npos || dot == 0 || candidate.size() > 255 ||
            candidate.find("..") != std::string_view::npos)
            return;
        const std::string_view extension = candidate.substr(dot + 1);
        if (extension.empty() || extension.size() > 8 ||
            !std::all_of(extension.begin(), extension.end(), [](const unsigned char character) { return std::isalnum(character) != 0; }))
            return;
        if (std::find(names.begin(), names.end(), candidate) == names.end())
            names.emplace_back(candidate);
    };
    const auto inName = [](const unsigned char character)
    { return std::isalnum(character) != 0 || character == '_' || character == '-' || character == '.' || character >= 0x80; };
    for (std::size_t at = 0; at < plain.size();)
    {
        // Quoted, a name may hold spaces. What is inside is still read word by word after.
        if (plain[at] == '"' || plain[at] == '`')
        {
            const std::size_t close = plain.find(plain[at], at + 1);
            if (close != std::string::npos && plain.find_first_of("\r\n/\\", at + 1) >= close)
                add(std::string_view(plain).substr(at + 1, close - at - 1));
            ++at;
            continue;
        }
        if (!inName(static_cast<unsigned char>(plain[at])))
        {
            ++at;
            continue;
        }
        std::size_t end = at;
        while (end < plain.size() && inName(static_cast<unsigned char>(plain[end])))
            ++end;
        add(std::string_view(plain).substr(at, end - at));
        at = end;
    }
    return names;
}

// What is at `path`, asked of the filesystem without following the request any further.
enum class Found { Nothing, File, Folder };
Found Look(const std::filesystem::path& path)
{
    std::error_code failed;
    const auto status = std::filesystem::status(path, failed);
    if (failed)
        return Found::Nothing;
    return std::filesystem::is_directory(status) ? Found::Folder : std::filesystem::is_regular_file(status) ? Found::File : Found::Nothing;
}

// The folders the person has pointed at, newest first: each folder they named inside an
// approved root, and the folder of each file they named there.
std::vector<std::filesystem::path> NamedFolders(const std::vector<std::string_view>& messages, const std::vector<std::filesystem::path>& roots)
{
    constexpr std::size_t MaximumFolders = 4;
    std::vector<std::filesystem::path> folders;
    std::vector<std::string> seen;
    for (const std::string_view message : messages)
    {
        for (const auto& [begin, end] : AuthoredPathSpans(message))
        {
            const std::string authored(message.substr(begin, end - begin));
            if (folders.size() >= MaximumFolders || authored.find("..") != std::string::npos || !InsideAny(Folded(authored), roots))
                continue;
            std::filesystem::path place = actions::Utf8ToPath(authored);
            const Found found = Look(place);
            if (found == Found::Nothing)
                continue;
            if (found == Found::File)
                place = place.parent_path();
            const std::string folded = Folded(actions::PathToUtf8(place));
            if (std::find(seen.begin(), seen.end(), folded) != seen.end())
                continue;
            seen.push_back(folded);
            folders.push_back(std::move(place));
        }
    }
    return folders;
}
} // namespace

std::optional<actions::ActionOutcome> SubmitChatAction(
    const ChatActionPort& port, actions::ActionRequest request, const ToolCallContext& context, std::string& refusal)
{
    refusal.clear();
    const auto refuse = [&refusal](std::string reason)
    {
        refusal = std::move(reason);
        return std::optional<actions::ActionOutcome>{};
    };
    if (!port.evaluate || !port.execute)
        return refuse("The action runtime is not available.");
    if (!context.Admitted())
        return refuse("The turn ended before this tool ran.");
    request.requestedBy = "conversation_tool";
    // Rechecked at the executor's own effect boundary, after any approval wait.
    request.beforeEffect = [context, earlier = std::move(request.beforeEffect)](const std::string& target)
    {
        if (!context.Admitted())
            return std::string("The conversation turn that asked for this is no longer current.");
        return earlier ? earlier(target) : std::string{};
    };

    const actions::PolicyDecision decision = port.evaluate(request);
    bool confirmed = false;
    if (decision.verdict == actions::PolicyVerdict::RequiresConfirmation)
    {
        if (!port.confirm)
        {
            return refuse("This needs your approval, and no approval prompt is available in this conversation, so nothing "
                          "was done. Policy said: " +
                          decision.reason);
        }
        if (!actions::Granted(port.confirm(request, decision)))
            return refuse("The owner declined this action, so nothing was done.");
        if (!context.Admitted())
            return refuse("The turn ended while approval was being asked for.");
        confirmed = true;
    }
    // A blocked request still goes to the runtime: it refuses before any executor and
    // records the refusal in the action audit, as every rejected action must be.
    actions::ActionOutcome outcome = port.execute(request, confirmed);
    if (outcome.policy.reason.empty())
        outcome.policy.reason = decision.reason;
    return outcome;
}

ToolExecution RunChatAction(const ChatActionPort& port, actions::ActionRequest request, const ToolCallContext& context)
{
    std::string refusal;
    const auto submitted = SubmitChatAction(port, std::move(request), context, refusal);
    if (!submitted)
        return ToolExecution::Fail(refusal);
    const actions::ActionOutcome& outcome = *submitted;
    const actions::PolicyDecision& decision = outcome.policy;
    if (!outcome.Succeeded())
        return ToolExecution::Fail(outcome.Message().empty() ? decision.reason : outcome.Message());
    std::string content;
    for (const auto& entry : outcome.result.entries)
        content += entry + "\n";
    content += outcome.result.content;
    if (content.empty())
        content = outcome.result.message;
    return ToolExecution::Ok(std::move(content));
}

actions::ActionRequest ChatFileRequest(const actions::ActionType type, const std::string& path)
{
    actions::ActionRequest request;
    request.id = actions::NewActionId();
    request.type = type;
    request.source = actions::Utf8ToPath(path);
    return request;
}

std::vector<std::string> AuthoredPaths(const std::string_view text)
{
    std::vector<std::string> paths;
    for (const auto& [begin, end] : AuthoredPathSpans(text))
        paths.emplace_back(text.substr(begin, end - begin));
    return paths;
}

std::vector<llm::ToolCall> OpeningFileCalls(
    const std::string_view text, const std::vector<std::filesystem::path>& approvedRoots, const std::vector<std::string>& earlier)
{
    constexpr std::size_t MaximumOpeningReads = 2;
    std::vector<llm::ToolCall> calls;
    if (approvedRoots.empty())
        return calls;
    std::vector<std::string> seen;
    const auto read = [&](const std::string& path, const bool folder)
    {
        const std::string folded = Folded(path);
        if (calls.size() >= MaximumOpeningReads || std::find(seen.begin(), seen.end(), folded) != seen.end())
            return;
        seen.push_back(folded);
        calls.push_back({"opening_" + std::to_string(calls.size() + 1), folder ? "list_dir" : "read_file",
            nlohmann::json{{"path", path}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)});
    };

    // A full path, read as it was written.
    const std::vector<Span> spans = AuthoredPathSpans(text);
    for (const auto& [begin, end] : spans)
    {
        const std::string authored(text.substr(begin, end - begin));
        // A path that climbs with ".." is for policy to judge if the model asks for it.
        if (authored.find("..") != std::string::npos || !InsideAny(Folded(authored), approvedRoots))
            continue;
        const Found found = Look(actions::Utf8ToPath(authored));
        if (found != Found::Nothing)
            read(authored, found == Found::Folder);
    }

    // A file named on its own: "todo.md in that folder". It is looked for in the folders
    // the person has pointed at, newest first, and then in the approved roots themselves,
    // where it has to be in one only: in two, there is no saying which was meant.
    const std::vector<std::string> names = AuthoredFileNames(text, spans);
    if (names.empty() || calls.size() >= MaximumOpeningReads)
        return calls;
    std::vector<std::string_view> messages = {text};
    messages.insert(messages.end(), earlier.begin(), earlier.end());
    const std::vector<std::filesystem::path> pointedAt = NamedFolders(messages, approvedRoots);
    for (const std::string& name : names)
    {
        const std::filesystem::path file = actions::Utf8ToPath(name);
        const auto holds = [&file](const std::filesystem::path& folder) { return Look(folder / file) == Found::File; };
        auto place = std::find_if(pointedAt.begin(), pointedAt.end(), holds);
        if (place == pointedAt.end())
        {
            place = std::find_if(approvedRoots.begin(), approvedRoots.end(), holds);
            if (place == approvedRoots.end() || std::find_if(place + 1, approvedRoots.end(), holds) != approvedRoots.end())
                continue;
        }
        read(actions::PathToUtf8(*place / file), false);
    }
    return calls;
}

llm::ToolCall RequestedDelegationCall(const std::string_view request)
{
    return {"requested_1", "delegate_task",
        nlohmann::json{{"objective", utf8::Prefix(utf8::Sanitize(request), MaximumDelegatedObjectiveCharacters)},
            {"reason", "The person asked for the agents, and the reply did not hand the work to them."}}
            .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)};
}

ToolExecution AddChatReminder(
    planning::ReminderBook& book, const std::string& text, const std::string& when, const planning::WallClock::time_point now)
{
    planning::ReminderRequest request;
    std::string error;
    switch (planning::ParseReminderRequest("remind me " + when + " to " + text, now, request, error))
    {
    case planning::ReminderParse::NotAReminder:
        return ToolExecution::Fail("That time could not be read. Use words such as 'in 10 minutes', 'at 3pm' or 'tomorrow at 9am'.");
    case planning::ReminderParse::Invalid:
        return ToolExecution::Fail(error);
    case planning::ReminderParse::Parsed:
        break;
    }
    std::string saveError;
    const auto added = book.Add(request, saveError);
    if (!added)
        return ToolExecution::Fail(saveError.empty() ? "The reminder list is full." : saveError);
    std::string confirmation = "Reminder set: " + planning::Label(request) + ", due " + planning::DescribeWhen(request.due, now) + " (in " +
                               planning::DescribeSpan(request.due - now) + ").";
    if (!saveError.empty())
        confirmation += " It could not be saved to disk, so it lasts only until Revia closes: " + saveError;
    return ToolExecution::Ok(std::move(confirmation));
}

} // namespace revia::agents
