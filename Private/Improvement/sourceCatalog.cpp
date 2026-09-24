#include "Improvement/sourceCatalog.h"

#include "Actions/actionTypes.h"
#include "Core/utf8.h"
#include "Improvement/codeProposal.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

namespace revia::improvement
{

namespace
{

constexpr std::uintmax_t MaximumReviewableBytes = 1024 * 1024;

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

// Letters and digits only, lowered: "Qwen3-TTS" and "qwenTtsPool" meet as "qwen3tts" and
// "qwenttspool".
std::string Squash(const std::string& text)
{
    std::string squashed;
    for (const unsigned char character : text)
    {
        if (std::isalnum(character) != 0)
            squashed.push_back(static_cast<char>(std::tolower(character)));
    }
    return squashed;
}

bool EndsWith(const std::string& text, const std::string& suffix)
{
    return text.size() >= suffix.size() &&
        text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Component names self-assessment uses whose files are named differently.
const std::map<std::string, std::vector<std::string>>& Aliases()
{
    static const std::map<std::string, std::vector<std::string>> aliases = {
        {"qwen3tts", {"qwentts", "qwen_tts", "speechservice"}},
        {"voice", {"speechservice", "qwentts", "speechcoordinator"}},
        {"tts", {"qwentts", "speechservice"}},
        {"main", {"llamacppservice", "conversationruntime", "messagerouter"}},
        {"expert", {"messagerouter", "modellifetime", "llamacppservice"}},
        {"llamacpp", {"llamacppservice", "llamacppserverprocess"}},
        {"memory", {"memoryagent", "longtermmemory", "llamacppservice"}},
        {"browser", {"visiblebrowser", "internetlookup"}},
        {"internet", {"internetlookup", "visiblebrowser"}},
        {"whisper", {"speechrecognition", "whisperserverprocess"}},
        {"speechrecognition", {"speechrecognition", "whisperserverprocess"}},
        {"conversation", {"conversationruntime", "reviasession"}}};
    return aliases;
}

} // namespace

SourceCatalog::SourceCatalog(std::filesystem::path inputRoot) : root(std::move(inputRoot)) {}

std::optional<std::filesystem::path> SourceCatalog::LocateSourceRoot(
    const std::filesystem::path& start)
{
    std::error_code error;
    std::filesystem::path current = std::filesystem::absolute(start, error);
    if (error) return std::nullopt;
    for (int depth = 0; depth < 8 && !current.empty(); ++depth)
    {
        if (std::filesystem::is_regular_file(current / "CMakeLists.txt", error) &&
            std::filesystem::is_directory(current / "Private", error) &&
            std::filesystem::is_directory(current / "Public", error))
        {
            return current;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return std::nullopt;
}

bool SourceCatalog::IsReviewable(const std::string& relativePath)
{
    if (relativePath.empty() || relativePath.size() > 260 || relativePath.front() == '/' ||
        relativePath.find('\\') != std::string::npos ||
        relativePath.find(':') != std::string::npos ||
        relativePath.find("..") != std::string::npos ||
        relativePath.find("//") != std::string::npos)
    {
        return false;
    }
    for (const unsigned char character : relativePath)
    {
        if (std::iscntrl(character) != 0) return false;
    }
    const bool code = EndsWith(relativePath, ".cpp") || EndsWith(relativePath, ".h");
    if (code && (relativePath.rfind("Private/", 0) == 0 ||
            relativePath.rfind("Public/", 0) == 0 || relativePath.rfind("Desktop/", 0) == 0))
    {
        return true;
    }
    // The voice worker itself, not the scripts that install or build things.
    return relativePath.rfind("Tools/", 0) == 0 && EndsWith(relativePath, ".py") &&
        relativePath.find('/', 6) == std::string::npos;
}

std::vector<std::string> SourceCatalog::Files() const
{
    std::vector<std::string> files;
    if (!Valid()) return files;
    std::error_code error;
    for (const char* folder : {"Private", "Public", "Desktop", "Tools"})
    {
        const std::filesystem::path base = root / folder;
        if (!std::filesystem::is_directory(base, error)) continue;
        auto iterator = std::filesystem::recursive_directory_iterator(
            base, std::filesystem::directory_options::skip_permission_denied, error);
        for (; !error && iterator != std::filesystem::recursive_directory_iterator();
            iterator.increment(error))
        {
            if (!iterator->is_regular_file(error)) continue;
            const std::string relative = actions::PathToUtf8(
                std::filesystem::relative(iterator->path(), root, error));
            if (!error && IsReviewable(relative)) files.push_back(relative);
        }
        error.clear();
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool SourceCatalog::Read(
    const std::string& relativePath, std::string& outContent, std::string& outError) const
{
    outContent.clear();
    if (!Valid() || !IsReviewable(relativePath))
    {
        outError = relativePath + " is not one of the files she reviews.";
        return false;
    }
    const std::filesystem::path path = root / actions::Utf8ToPath(relativePath);
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error)
    {
        outError = relativePath + " does not exist.";
        return false;
    }
    if (size > MaximumReviewableBytes)
    {
        outError = relativePath + " is too large to review.";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file.good() && !file.eof())
    {
        outError = relativePath + " could not be read.";
        return false;
    }
    std::string content = NormalizeLineEndings(buffer.str());
    // Everything she reads is sent to a model as JSON, which refuses malformed UTF-8. A
    // file saved in a legacy code page is simply not one she can review.
    if (!utf8::IsValid(content))
    {
        outError = relativePath + " is not UTF-8 text.";
        return false;
    }
    outContent = std::move(content);
    outError.clear();
    return true;
}

std::vector<std::string> SourceCatalog::FilesFor(
    const std::vector<std::string>& components,
    const std::vector<std::string>& metrics,
    const std::size_t maximum) const
{
    const std::vector<std::string> files = Files();
    std::map<std::string, int> score;
    for (const std::string& component : components)
    {
        std::vector<std::string> keys{Squash(component)};
        if (const auto alias = Aliases().find(keys.front()); alias != Aliases().end())
            keys.insert(keys.end(), alias->second.begin(), alias->second.end());
        for (const std::string& key : keys)
        {
            if (key.size() < 3) continue;
            for (const std::string& file : files)
            {
                const std::string name = Squash(std::filesystem::path(file).stem().string());
                if (name.find(Squash(key)) != std::string::npos) score[file] += 3;
            }
        }
    }
    // A metric is named where it is measured, which is where the slow thing happens.
    for (const std::string& metric : metrics)
    {
        if (metric.size() < 4) continue;
        const std::string quoted = "\"" + metric + "\"";
        for (const std::string& file : files)
        {
            std::string content;
            std::string ignored;
            if (Read(file, content, ignored) && content.find(quoted) != std::string::npos)
                score[file] += 2;
        }
    }
    std::vector<std::pair<std::string, int>> ranked(score.begin(), score.end());
    // Implementation before declaration: the .cpp is where the behaviour lives.
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right)
    {
        if (left.second != right.second) return left.second > right.second;
        return EndsWith(left.first, ".cpp") && !EndsWith(right.first, ".cpp");
    });
    std::vector<std::string> result;
    for (const auto& [file, points] : ranked)
    {
        (void)points;
        if (result.size() >= maximum) break;
        result.push_back(file);
    }
    return result;
}

std::vector<std::string> SourceCatalog::Resolve(const std::string& target) const
{
    std::string wanted = target;
    std::replace(wanted.begin(), wanted.end(), '\\', '/');
    while (!wanted.empty() && std::isspace(static_cast<unsigned char>(wanted.back())) != 0)
        wanted.pop_back();
    while (!wanted.empty() && std::isspace(static_cast<unsigned char>(wanted.front())) != 0)
        wanted.erase(0, 1);
    if (wanted.empty()) return {};
    if (IsReviewable(wanted))
    {
        std::string content;
        std::string ignored;
        if (Read(wanted, content, ignored)) return {wanted};
    }
    const std::vector<std::string> files = Files();
    std::vector<std::string> byName;
    const std::string lowered = Lower(wanted);
    for (const std::string& file : files)
    {
        const std::string name = Lower(std::filesystem::path(file).filename().string());
        const std::string stem = Lower(std::filesystem::path(file).stem().string());
        if (name == lowered || stem == lowered) byName.push_back(file);
    }
    if (!byName.empty())
    {
        std::stable_sort(byName.begin(), byName.end(), [](const auto& left, const auto& right)
        {
            return EndsWith(left, ".cpp") && !EndsWith(right, ".cpp");
        });
        return byName;
    }
    return FilesFor({wanted}, {}, 3);
}

CodeWindow ExtractWindow(
    const std::string& path,
    const std::string& content,
    const std::vector<std::string>& anchors,
    const std::size_t startLine,
    const std::size_t maximumLines,
    const std::size_t maximumCharacters)
{
    CodeWindow window;
    window.path = path;
    std::vector<std::string> lines;
    std::istringstream stream(content);
    for (std::string line; std::getline(stream, line);) lines.push_back(line);
    window.totalLines = lines.size();
    if (lines.empty() || maximumLines == 0) return window;

    std::size_t focus = std::min(std::max<std::size_t>(startLine, 1), lines.size()) - 1;
    bool anchored = false;
    for (const std::string& anchor : anchors)
    {
        if (anchor.size() < 3) continue;
        for (std::size_t index = 0; index < lines.size() && !anchored; ++index)
        {
            if (lines[index].find(anchor) != std::string::npos)
            {
                focus = index;
                anchored = true;
            }
        }
        if (anchored) break;
    }

    // An anchor sits a third of the way down, so the code leading into it is shown too.
    const std::size_t lead = maximumLines / 3;
    const std::size_t first = !anchored ? focus : focus > lead ? focus - lead : 0;
    std::size_t last = std::min(lines.size(), first + maximumLines) - 1;
    std::size_t characters = 0;
    for (std::size_t index = first; index <= last; ++index)
    {
        if (characters + lines[index].size() + 1 > maximumCharacters && index > first)
        {
            last = index - 1;
            break;
        }
        characters += lines[index].size() + 1;
        window.text += lines[index];
        window.text += '\n';
    }
    window.firstLine = first + 1;
    window.lastLine = last + 1;
    return window;
}

} // namespace revia::improvement
