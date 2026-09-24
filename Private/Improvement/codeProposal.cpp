#include "Improvement/codeProposal.h"

#include "Core/utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string_view>
#include <utility>

namespace revia::improvement
{

namespace
{

using json = nlohmann::json;

constexpr std::size_t MaximumSnippetCharacters = 4000;
constexpr std::size_t MaximumSnippetLines = 60;
constexpr std::size_t MaximumProseCharacters = 1200;

std::string RightTrim(std::string_view line)
{
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
    {
        line.remove_suffix(1);
    }
    return std::string(line);
}

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
        {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::size_t CountLines(const std::string& text)
{
    if (text.empty()) return 0;
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) +
        (text.back() == '\n' ? 0 : 1);
}

// Models wrap snippets in fences however firmly they are told not to.
std::string StripFences(std::string text)
{
    const auto trimmedStart = text.find_first_not_of(" \t\r\n");
    if (trimmedStart != std::string::npos && text.compare(trimmedStart, 3, "```") == 0)
    {
        const std::size_t lineEnd = text.find('\n', trimmedStart);
        text = lineEnd == std::string::npos ? std::string{} : text.substr(lineEnd + 1);
        const std::size_t close = text.rfind("```");
        if (close != std::string::npos) text.erase(close);
    }
    return text;
}

std::string Bounded(const json& data, const char* key, const std::size_t limit)
{
    if (!data.contains(key) || !data[key].is_string()) return {};
    std::string value = data[key].get<std::string>();
    utf8::Truncate(value, limit);
    return value;
}

double Unit(const json& data, const char* key)
{
    if (!data.contains(key) || !data[key].is_number()) return 0.0;
    return std::clamp(data[key].get<double>(), 0.0, 1.0);
}

// Where `find` sits in `content`, when it is there exactly once.
struct Location
{
    std::size_t begin = 0;
    std::size_t end = 0;
    // Indentation every line of `find` was missing, to be given to the replacement too.
    std::string indent;
};

// Exact first. Failing that, line by line with trailing whitespace ignored, because a
// model reliably drops a trailing space and nothing about the edit depends on it. Leading
// whitespace is forgiven only as one shift shared by every line: a model copying a block
// out of a function often drops the indentation they all have in common, and that keeps
// the relative indentation, which in Python is the program.
std::optional<Location> LocateUnique(const std::string& content, const std::string& find)
{
    if (find.find_first_not_of(" \t\n") == std::string::npos) return std::nullopt;

    std::size_t exactCount = 0;
    std::size_t exactAt = std::string::npos;
    for (std::size_t at = content.find(find); at != std::string::npos;
        at = content.find(find, at + 1))
    {
        if (++exactCount == 1) exactAt = at;
        if (exactCount > 1) return std::nullopt;
    }
    if (exactCount == 1) return Location{exactAt, exactAt + find.size(), {}};

    std::string trimmedFind = find;
    while (!trimmedFind.empty() && trimmedFind.back() == '\n') trimmedFind.pop_back();
    std::vector<std::string> wanted = SplitLines(trimmedFind);
    for (std::string& line : wanted) line = RightTrim(line);
    const std::vector<std::string> lines = SplitLines(content);
    if (wanted.empty() || wanted.size() > lines.size()) return std::nullopt;

    std::vector<std::size_t> lineStarts;
    lineStarts.reserve(lines.size());
    std::size_t offset = 0;
    for (const std::string& line : lines)
    {
        lineStarts.push_back(offset);
        offset += line.size() + 1;
    }

    std::size_t firstText = 0;
    while (firstText < wanted.size() && wanted[firstText].empty()) ++firstText;
    if (firstText == wanted.size()) return std::nullopt;

    std::size_t matches = 0;
    std::size_t matchLine = 0;
    std::string matchIndent;
    for (std::size_t first = 0; first + wanted.size() <= lines.size(); ++first)
    {
        // The shift, if any, is whatever the first line with text is missing.
        const std::string anchor = RightTrim(lines[first + firstText]);
        const std::string& expected = wanted[firstText];
        if (anchor.size() < expected.size() ||
            anchor.compare(anchor.size() - expected.size(), std::string::npos, expected) != 0)
        {
            continue;
        }
        const std::string indent = anchor.substr(0, anchor.size() - expected.size());
        if (indent.find_first_not_of(" \t") != std::string::npos) continue;
        bool same = true;
        for (std::size_t index = 0; index < wanted.size() && same; ++index)
        {
            const std::string line = RightTrim(lines[first + index]);
            same = wanted[index].empty() ? line.empty() : line == indent + wanted[index];
        }
        if (same)
        {
            if (++matches > 1) return std::nullopt;
            matchLine = first;
            matchIndent = indent;
        }
    }
    if (matches != 1) return std::nullopt;
    const std::size_t last = matchLine + wanted.size() - 1;
    const std::size_t end = std::min(content.size(), lineStarts[last] + lines[last].size());
    return Location{lineStarts[matchLine], end, matchIndent};
}

// Calls that reach outside the process. Checked only on what a change adds, so editing
// code that already starts the voice worker is fine and adding a new launch is not.
constexpr std::string_view OutsideReach[] = {
    // Programs.
    "system(", "popen(", "_wsystem", "CreateProcess", "ShellExecute", "WinExec",
    "QProcess", "execv", "execl", "execvp", "spawnl", "_spawn",
    // Code loaded at run time.
    "LoadLibrary", "GetProcAddress", "dlopen", "__asm", "asm(", "asm volatile",
    // Files and folders destroyed.
    "remove_all", "filesystem::remove", "DeleteFile", "RemoveDirectory", "SHFileOperation",
    // The registry and the process itself.
    "RegSetValue", "RegCreateKey", "RegDeleteKey", "ExitProcess", "TerminateProcess",
    // The network.
    "httplib::Client", "httplib::SSLClient", "WSAStartup", "URLDownload", "InternetOpen",
    "WinHttp",
    // The same, in the Python the voice worker is written in.
    "subprocess", "os.system", "os.popen", "os.remove", "os.unlink", "shutil.rmtree",
    "eval(", "exec(", "__import__", "urllib"};

// Whitespace removed, so "std::system (" and "asm  volatile" read as the tokens above.
std::string WithoutWhitespace(std::string_view text)
{
    std::string compact;
    compact.reserve(text.size());
    for (const char character : text)
    {
        if (character != ' ' && character != '\t' && character != '\n' && character != '\r' &&
            character != '\v' && character != '\f')
        {
            compact.push_back(character);
        }
    }
    return compact;
}

std::size_t CountOccurrences(const std::string& text, const std::string& token)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(token); at != std::string::npos;
        at = text.find(token, at + token.size()))
    {
        ++count;
    }
    return count;
}

std::string Hex(const std::uint64_t value)
{
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string ForFingerprint(const std::string& text)
{
    std::vector<std::string> lines = SplitLines(NormalizeLineEndings(text));
    std::string joined;
    for (const std::string& line : lines)
    {
        joined += RightTrim(line);
        joined += '\n';
    }
    return joined;
}

} // namespace

std::string ToString(const ProposalStatus status)
{
    switch (status)
    {
        case ProposalStatus::Drafted: return "drafted";
        case ProposalStatus::Verified: return "verified";
        case ProposalStatus::FailedVerification: return "failed";
        case ProposalStatus::Accepted: return "accepted";
        case ProposalStatus::Rejected: return "rejected";
    }
    return "drafted";
}

std::optional<ProposalStatus> ProposalStatusFromString(const std::string& text)
{
    for (const ProposalStatus status : {ProposalStatus::Drafted, ProposalStatus::Verified,
             ProposalStatus::FailedVerification, ProposalStatus::Accepted,
             ProposalStatus::Rejected})
    {
        if (ToString(status) == text) return status;
    }
    return std::nullopt;
}

std::string NormalizeLineEndings(std::string text)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') continue;
        output.push_back(text[index]);
    }
    return output;
}

std::string Fingerprint(const CodeChange& change)
{
    // FNV-1a: stable across runs and builds, which std::hash is not required to be.
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](const std::string& part)
    {
        for (const unsigned char character : part)
        {
            hash ^= character;
            hash *= 1099511628211ULL;
        }
        hash ^= 0xFF;
        hash *= 1099511628211ULL;
    };
    mix(change.path);
    mix(ForFingerprint(change.find));
    mix(ForFingerprint(change.replace));
    return Hex(hash);
}

json ToJson(const CodeProposal& proposal)
{
    return {
        {"id", proposal.id},
        {"createdAt", proposal.createdAt},
        {"trigger", proposal.trigger},
        {"taskId", proposal.taskId},
        {"title", proposal.title},
        {"problem", proposal.problem},
        {"reason", proposal.reason},
        {"evidence", proposal.evidence},
        {"expectedBenefit", proposal.expectedBenefit},
        {"risks", proposal.risks},
        {"benefit", proposal.benefit},
        {"risk", proposal.risk},
        {"change", {
            {"path", proposal.change.path},
            {"find", proposal.change.find},
            {"replace", proposal.change.replace}}},
        {"status", ToString(proposal.status)},
        {"verificationSummary", proposal.verificationSummary},
        {"feedback", proposal.feedback},
        {"fingerprint", proposal.fingerprint}};
}

std::optional<CodeProposal> ProposalFromJson(const json& data)
{
    if (!data.is_object() || !data.contains("id") || !data["id"].is_string() ||
        !data.contains("change") || !data["change"].is_object())
    {
        return std::nullopt;
    }
    CodeProposal proposal;
    const auto text = [&data](const char* key)
    {
        return data.contains(key) && data[key].is_string() ? data[key].get<std::string>()
                                                           : std::string{};
    };
    proposal.id = text("id");
    proposal.createdAt = text("createdAt");
    proposal.trigger = text("trigger");
    proposal.taskId = text("taskId");
    proposal.title = text("title");
    proposal.problem = text("problem");
    proposal.reason = text("reason");
    proposal.evidence = text("evidence");
    proposal.expectedBenefit = text("expectedBenefit");
    proposal.risks = text("risks");
    proposal.benefit = Unit(data, "benefit");
    proposal.risk = Unit(data, "risk");
    const json& change = data["change"];
    if (!change.contains("path") || !change["path"].is_string() ||
        !change.contains("find") || !change["find"].is_string() ||
        !change.contains("replace") || !change["replace"].is_string())
    {
        return std::nullopt;
    }
    proposal.change.path = change["path"].get<std::string>();
    proposal.change.find = change["find"].get<std::string>();
    proposal.change.replace = change["replace"].get<std::string>();
    proposal.status = ProposalStatusFromString(text("status")).value_or(ProposalStatus::Drafted);
    proposal.verificationSummary = text("verificationSummary");
    proposal.feedback = text("feedback");
    proposal.fingerprint = text("fingerprint");
    if (proposal.fingerprint.empty()) proposal.fingerprint = Fingerprint(proposal.change);
    return proposal;
}

std::optional<CodeProposal> ParseReviewReply(const std::string& raw, std::string& outNote)
{
    const std::size_t open = raw.find('{');
    const std::size_t close = raw.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close < open)
    {
        outNote = "The review did not come back as JSON.";
        return std::nullopt;
    }
    json data;
    try
    {
        data = json::parse(raw.substr(open, close - open + 1));
    }
    catch (const std::exception& error)
    {
        outNote = std::string("The review was not valid JSON: ") + error.what();
        return std::nullopt;
    }
    if (!data.is_object() || !data.contains("found") || !data["found"].is_boolean())
    {
        outNote = "The review did not say whether it found anything.";
        return std::nullopt;
    }
    if (!data["found"].get<bool>())
    {
        const std::string why = Bounded(data, "reason", MaximumProseCharacters);
        outNote = why.empty() ? "She found nothing worth changing." : why;
        return std::nullopt;
    }

    CodeProposal proposal;
    proposal.title = Bounded(data, "title", 160);
    proposal.problem = Bounded(data, "problem", MaximumProseCharacters);
    proposal.reason = Bounded(data, "reason", MaximumProseCharacters);
    proposal.evidence = Bounded(data, "evidence", MaximumProseCharacters);
    proposal.expectedBenefit = Bounded(data, "expected_benefit", MaximumProseCharacters);
    proposal.risks = Bounded(data, "risks", MaximumProseCharacters);
    proposal.benefit = Unit(data, "benefit");
    proposal.risk = Unit(data, "risk");
    proposal.change.path = Bounded(data, "file", 260);
    std::replace(proposal.change.path.begin(), proposal.change.path.end(), '\\', '/');
    // Unbounded here on purpose: a snippet cut to fit would silently become a different
    // edit. CheckChange refuses an oversized one outright instead.
    if (data.contains("find") && data["find"].is_string())
        proposal.change.find = NormalizeLineEndings(StripFences(data["find"].get<std::string>()));
    if (data.contains("replace") && data["replace"].is_string())
        proposal.change.replace =
            NormalizeLineEndings(StripFences(data["replace"].get<std::string>()));

    std::string missing;
    for (const auto& [name, value] : {std::pair<const char*, const std::string*>{"title", &proposal.title},
             {"problem", &proposal.problem}, {"reason", &proposal.reason},
             {"file", &proposal.change.path}, {"find", &proposal.change.find}})
    {
        if (value->empty()) missing += (missing.empty() ? "" : ", ") + std::string(name);
    }
    if (!missing.empty())
    {
        outNote = "The review claimed a finding but left out: " + missing + ".";
        return std::nullopt;
    }
    proposal.fingerprint = Fingerprint(proposal.change);
    outNote.clear();
    return proposal;
}

ChangeCheck CheckChange(const CodeChange& change, const std::string& currentContent)
{
    const std::string content = NormalizeLineEndings(currentContent);
    const std::string find = NormalizeLineEndings(change.find);
    const std::string replace = NormalizeLineEndings(change.replace);
    if (find.size() > MaximumSnippetCharacters || replace.size() > MaximumSnippetCharacters ||
        CountLines(find) > MaximumSnippetLines || CountLines(replace) > MaximumSnippetLines)
    {
        return {false, "The change is larger than one focused edit (over " +
            std::to_string(MaximumSnippetLines) + " lines)."};
    }
    if (ForFingerprint(find) == ForFingerprint(replace))
    {
        return {false, "The replacement is the same as the original."};
    }
    if (!LocateUnique(content, find))
    {
        return {false, "The code to replace is not in the file exactly once, so the edit "
            "would land somewhere unintended or nowhere.", true};
    }
    // Counted, not merely present: code that already launches one process must not gain a
    // second launch just because the first is inside the edited lines.
    const std::string compactFind = WithoutWhitespace(find);
    const std::string compactReplace = WithoutWhitespace(replace);
    for (const std::string_view token : OutsideReach)
    {
        const std::string compactToken = WithoutWhitespace(token);
        if (CountOccurrences(compactReplace, compactToken) >
            CountOccurrences(compactFind, compactToken))
        {
            return {false, "The change adds '" + std::string(token) + "', which reaches "
                "outside the process; proposals may not introduce that."};
        }
    }
    return {true, {}};
}

bool Overlaps(const std::string& currentContent, const CodeChange& first, const CodeChange& second)
{
    if (first.path != second.path) return false;
    const std::string content = NormalizeLineEndings(currentContent);
    const auto one = LocateUnique(content, NormalizeLineEndings(first.find));
    const auto other = LocateUnique(content, NormalizeLineEndings(second.find));
    return one && other && one->begin < other->end && other->begin < one->end;
}

std::string FirstLineNotInFile(const std::string& currentContent, const std::string& find)
{
    const auto bare = [](const std::string& line)
    {
        const std::string trimmed = RightTrim(line);
        const std::size_t text = trimmed.find_first_not_of(" \t");
        return text == std::string::npos ? std::string{} : trimmed.substr(text);
    };
    std::vector<std::string> present;
    for (const std::string& line : SplitLines(NormalizeLineEndings(currentContent)))
    {
        present.push_back(bare(line));
    }
    std::sort(present.begin(), present.end());
    for (const std::string& line : SplitLines(NormalizeLineEndings(find)))
    {
        const std::string wanted = bare(line);
        if (!wanted.empty() && !std::binary_search(present.begin(), present.end(), wanted))
        {
            return wanted;
        }
    }
    return {};
}

std::optional<std::string> ApplyChange(const std::string& currentContent, const CodeChange& change)
{
    const bool crlf = currentContent.find("\r\n") != std::string::npos;
    const std::string content = NormalizeLineEndings(currentContent);
    const std::string find = NormalizeLineEndings(change.find);
    const auto located = LocateUnique(content, find);
    if (!located) return std::nullopt;
    std::string replace = NormalizeLineEndings(change.replace);
    // The same shift for the replacement, unless its first line with text already has it.
    const std::size_t replaceText = replace.find_first_not_of(" \t\n");
    if (!located->indent.empty() && replaceText != std::string::npos)
    {
        const std::size_t lineBreak = replace.rfind('\n', replaceText);
        const std::size_t lineBegin = lineBreak == std::string::npos ? 0 : lineBreak + 1;
        if (replace.compare(lineBegin, located->indent.size(), located->indent) != 0)
        {
            std::string shifted;
            bool atLineStart = true;
            for (const char character : replace)
            {
                if (atLineStart && character != '\n') shifted += located->indent;
                shifted.push_back(character);
                atLineStart = character == '\n';
            }
            replace = std::move(shifted);
        }
    }
    // The replacement ends a line exactly when the text it replaces did. Models are
    // careless with a final newline, and the tolerant match excludes it, so trusting
    // the reply either joined two lines or left a stray blank one.
    const bool regionEndsLine = located->end > located->begin &&
        content[located->end - 1] == '\n';
    if (!replace.empty())
    {
        if (regionEndsLine && replace.back() != '\n') replace.push_back('\n');
        while (!regionEndsLine && !replace.empty() && replace.back() == '\n') replace.pop_back();
    }
    std::string result = content.substr(0, located->begin) + replace +
        content.substr(located->end);
    if (!crlf) return result;
    std::string restored;
    restored.reserve(result.size() + result.size() / 32);
    for (const char character : result)
    {
        if (character == '\n') restored.push_back('\r');
        restored.push_back(character);
    }
    return restored;
}

std::string MakeUnifiedDiff(const std::string& currentContent, const CodeChange& change)
{
    const std::optional<std::string> changed = ApplyChange(currentContent, change);
    if (!changed) return {};
    const std::string before = NormalizeLineEndings(currentContent);
    const std::string after = NormalizeLineEndings(*changed);
    const bool beforeEndsWithNewline = !before.empty() && before.back() == '\n';
    const bool afterEndsWithNewline = !after.empty() && after.back() == '\n';
    std::vector<std::string> oldLines = SplitLines(before);
    std::vector<std::string> newLines = SplitLines(after);
    if (beforeEndsWithNewline) oldLines.pop_back();
    if (afterEndsWithNewline) newLines.pop_back();

    std::size_t prefix = 0;
    while (prefix < oldLines.size() && prefix < newLines.size() &&
        oldLines[prefix] == newLines[prefix])
    {
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < oldLines.size() - prefix && suffix < newLines.size() - prefix &&
        oldLines[oldLines.size() - 1 - suffix] == newLines[newLines.size() - 1 - suffix])
    {
        ++suffix;
    }
    // The last line of a file that loses or gains its final newline differs even when
    // its text does not; keep it inside the hunk so the marker lands where git wants it.
    if (beforeEndsWithNewline != afterEndsWithNewline && suffix > 0) --suffix;

    constexpr std::size_t Context = 3;
    const std::size_t start = prefix > Context ? prefix - Context : 0;
    const std::size_t oldEnd = std::min(oldLines.size(), oldLines.size() - suffix + Context);
    const std::size_t newEnd = std::min(newLines.size(), newLines.size() - suffix + Context);
    const std::size_t oldCount = oldEnd - start;
    const std::size_t newCount = newEnd - start;

    std::ostringstream diff;
    diff << "diff --git a/" << change.path << " b/" << change.path << "\n"
         << "--- a/" << change.path << "\n"
         << "+++ b/" << change.path << "\n"
         << "@@ -" << (oldCount == 0 ? start : start + 1) << ',' << oldCount
         << " +" << (newCount == 0 ? start : start + 1) << ',' << newCount << " @@\n";
    for (std::size_t index = start; index < prefix; ++index)
    {
        diff << ' ' << oldLines[index] << '\n';
    }
    const std::size_t oldChangedEnd = oldLines.size() - suffix;
    const std::size_t newChangedEnd = newLines.size() - suffix;
    for (std::size_t index = prefix; index < oldChangedEnd; ++index)
    {
        diff << '-' << oldLines[index] << '\n';
        if (index + 1 == oldLines.size() && !beforeEndsWithNewline)
            diff << "\\ No newline at end of file\n";
    }
    for (std::size_t index = prefix; index < newChangedEnd; ++index)
    {
        diff << '+' << newLines[index] << '\n';
        if (index + 1 == newLines.size() && !afterEndsWithNewline)
            diff << "\\ No newline at end of file\n";
    }
    for (std::size_t index = oldChangedEnd; index < oldEnd; ++index)
    {
        diff << ' ' << oldLines[index] << '\n';
        if (index + 1 == oldLines.size() && !beforeEndsWithNewline)
            diff << "\\ No newline at end of file\n";
    }
    return diff.str();
}

std::string ToMarkdown(const CodeProposal& proposal, const std::string& diff)
{
    std::ostringstream text;
    const std::string trigger = proposal.trigger == "evidence"
        ? "a problem she measured in herself" + (proposal.taskId.empty()
            ? std::string{} : " (self-assessment task " + proposal.taskId + ")")
        : proposal.trigger == "request" ? std::string("your request")
                                        : std::string("her own review while idle");
    char scores[64] = {};
    std::snprintf(scores, sizeof(scores), "%.2f / %.2f", proposal.benefit, proposal.risk);
    text << "# " << proposal.title << "\n\n"
         << "- **Id:** `" << proposal.id << "`\n"
         << "- **Status:** " << ToString(proposal.status)
         << (proposal.verificationSummary.empty() ? "" : " - " + proposal.verificationSummary)
         << "\n"
         << "- **Found through:** " << trigger << "\n"
         << "- **File:** `" << proposal.change.path << "`\n"
         << "- **Benefit / risk (her estimate):** " << scores << "\n\n"
         << "## Problem\n\n" << proposal.problem << "\n\n"
         << "## Why this change\n\n" << proposal.reason << "\n\n";
    if (!proposal.evidence.empty()) text << "## Evidence\n\n" << proposal.evidence << "\n\n";
    if (!proposal.expectedBenefit.empty())
        text << "## Expected benefit\n\n" << proposal.expectedBenefit << "\n\n";
    if (!proposal.risks.empty()) text << "## Risks\n\n" << proposal.risks << "\n\n";
    text << "## Change\n\n```diff\n" << diff << "```\n\n"
         << "Apply from the repository root with `git apply " << proposal.id << ".patch`.\n";
    if (!proposal.feedback.empty()) text << "\n## Your verdict\n\n" << proposal.feedback << "\n";
    return text.str();
}

} // namespace revia::improvement
