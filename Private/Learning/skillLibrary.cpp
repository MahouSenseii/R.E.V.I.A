#include "Learning/skillLibrary.h"

#include "Actions/actionTypes.h"
#include "Core/utf8.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

namespace revia::learning
{

namespace
{
using nlohmann::json;

const std::set<std::string>& StopWords()
{
    static const std::set<std::string> words = {
        "a", "an", "the", "to", "of", "in", "on", "for", "and", "or", "my", "me", "please",
        "can", "you", "could", "would", "it", "this", "that", "with", "into", "from", "then",
        "revia", "i", "want", "need", "make", "do"};
    return words;
}

std::set<std::string> Words(const std::string& text)
{
    std::set<std::string> words;
    std::string current;
    const auto flush = [&]
    {
        if (current.size() >= 2 && StopWords().count(current) == 0) words.insert(current);
        current.clear();
    };
    for (const unsigned char character : text)
    {
        if (std::isalnum(character) != 0) current.push_back(static_cast<char>(std::tolower(character)));
        else flush();
    }
    flush();
    return words;
}

std::int64_t ToMilliseconds(const std::chrono::system_clock::time_point at)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

std::chrono::system_clock::time_point FromMilliseconds(const std::int64_t milliseconds)
{
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(milliseconds)));
}
} // namespace

double SkillLibrary::Similarity(const std::string& left, const std::string& right)
{
    const std::set<std::string> a = Words(left);
    const std::set<std::string> b = Words(right);
    if (a.empty() || b.empty()) return 0.0;
    std::size_t shared = 0;
    for (const std::string& word : a) shared += b.count(word);
    const std::size_t all = a.size() + b.size() - shared;
    return all == 0 ? 0.0 : static_cast<double>(shared) / static_cast<double>(all);
}

std::string SkillLibrary::DescribeStep(const goals::GoalStep& step)
{
    std::string text = step.description;
    std::string how = actions::ToString(step.action.type);
    if (!step.action.application.empty()) how += " in " + step.action.application;
    else if (!step.action.source.empty()) how += " " + actions::PathToUtf8(step.action.source.filename());
    if (!step.action.control.empty()) how += ", control " + step.action.control;
    if (text.empty()) text = how;
    else text += " (" + how + ")";
    return utf8::Prefix(text, LongestStep);
}

bool SkillLibrary::Initialize(const std::filesystem::path& file, std::string& outError)
{
    std::lock_guard lock(mutex);
    path = file;
    procedures.clear();
    std::ifstream stream(path);
    if (!stream) return true;
    try
    {
        const json data = json::parse(stream);
        nextId = std::max<std::uint64_t>(1, data.value<std::uint64_t>("next_id", 1));
        for (const json& raw : data.value("procedures", json::array()))
        {
            if (!raw.is_object() || procedures.size() >= MaximumProcedures) continue;
            Procedure procedure;
            procedure.id = raw.value<std::uint64_t>("id", 0);
            procedure.title = utf8::Prefix(raw.value("title", ""), LongestTitle);
            for (const json& step : raw.value("steps", json::array()))
            {
                if (step.is_string() && procedure.steps.size() < MaximumSteps)
                    procedure.steps.push_back(utf8::Prefix(step.get<std::string>(), LongestStep));
            }
            procedure.successes = raw.value<std::uint32_t>("successes", 0);
            procedure.failures = raw.value<std::uint32_t>("failures", 0);
            procedure.createdAt = FromMilliseconds(raw.value<std::int64_t>("created_ms", 0));
            procedure.lastUsedAt = FromMilliseconds(raw.value<std::int64_t>("used_ms", 0));
            if (procedure.id == 0 || procedure.title.empty() || procedure.steps.empty()) continue;
            nextId = std::max(nextId, procedure.id + 1);
            procedures.push_back(std::move(procedure));
        }
    }
    catch (const std::exception& error)
    {
        procedures.clear();
        stream.close();
        std::filesystem::path unreadable = path;
        unreadable += ".unreadable";
        std::error_code ignored;
        std::filesystem::rename(path, unreadable, ignored);
        outError = std::string("the procedure library could not be read (") + error.what() + ")";
        return false;
    }
    return true;
}

std::optional<Procedure> SkillLibrary::Learn(const goals::Goal& finished)
{
    if (finished.status != goals::GoalStatus::Succeeded || finished.steps.empty() || finished.title.empty())
    {
        return std::nullopt;
    }
    std::lock_guard lock(mutex);
    const std::string title = utf8::Prefix(finished.title, LongestTitle);
    for (Procedure& existing : procedures)
    {
        if (Similarity(existing.title, title) >= 0.9)
        {
            ++existing.successes;
            existing.lastUsedAt = std::chrono::system_clock::now();
            std::string ignored;
            (void)SaveLocked(ignored);
            return existing;
        }
    }
    if (procedures.size() >= MaximumProcedures)
    {
        // The least trusted, oldest one makes room.
        const auto weakest = std::min_element(procedures.begin(), procedures.end(),
            [](const Procedure& left, const Procedure& right)
            {
                const long long leftScore = static_cast<long long>(left.successes) - left.failures;
                const long long rightScore = static_cast<long long>(right.successes) - right.failures;
                return leftScore != rightScore ? leftScore < rightScore : left.lastUsedAt < right.lastUsedAt;
            });
        procedures.erase(weakest);
    }
    Procedure procedure;
    procedure.id = nextId++;
    procedure.title = title;
    for (const goals::GoalStep& step : finished.steps)
    {
        if (procedure.steps.size() >= MaximumSteps) break;
        procedure.steps.push_back(DescribeStep(step));
    }
    procedure.successes = 1;
    procedures.push_back(procedure);
    std::string ignored;
    (void)SaveLocked(ignored);
    return procedure;
}

void SkillLibrary::RecordFailure(const std::string& request)
{
    std::lock_guard lock(mutex);
    bool changed = false;
    for (Procedure& procedure : procedures)
    {
        if (Similarity(procedure.title, request) >= MinimumSimilarity)
        {
            ++procedure.failures;
            changed = true;
        }
    }
    if (changed)
    {
        std::string ignored;
        (void)SaveLocked(ignored);
    }
}

std::vector<Procedure> SkillLibrary::Similar(const std::string& request, const std::size_t maximum) const
{
    std::lock_guard lock(mutex);
    std::vector<std::pair<double, const Procedure*>> scored;
    for (const Procedure& procedure : procedures)
    {
        if (!procedure.Trusted()) continue;
        const double similarity = Similarity(procedure.title, request);
        if (similarity >= MinimumSimilarity) scored.emplace_back(similarity, &procedure);
    }
    std::stable_sort(scored.begin(), scored.end(),
        [](const auto& left, const auto& right) { return left.first > right.first; });
    std::vector<Procedure> found;
    for (const auto& [similarity, procedure] : scored)
    {
        if (found.size() >= maximum) break;
        found.push_back(*procedure);
    }
    return found;
}

std::string SkillLibrary::RenderForPlanner(const std::vector<Procedure>& procedures)
{
    if (procedures.empty()) return {};
    std::ostringstream block;
    block << "Procedures from Revia's own record that worked for similar requests before. They "
             "are a starting point, not part of the request: reuse what fits, adapt what does "
             "not, and plan only what this request asks.";
    std::size_t number = 0;
    for (const Procedure& procedure : procedures)
    {
        block << "\n" << ++number << ". " << procedure.title << " (worked " << procedure.successes
              << (procedure.successes == 1 ? " time" : " times") << "):";
        for (const std::string& step : procedure.steps) block << "\n   - " << step;
    }
    return block.str();
}

std::vector<Procedure> SkillLibrary::Entries() const
{
    std::lock_guard lock(mutex);
    return procedures;
}

std::optional<Procedure> SkillLibrary::Forget(const std::size_t number)
{
    std::lock_guard lock(mutex);
    if (number == 0 || number > procedures.size()) return std::nullopt;
    Procedure removed = procedures[number - 1];
    procedures.erase(procedures.begin() + static_cast<std::ptrdiff_t>(number - 1));
    std::string ignored;
    (void)SaveLocked(ignored);
    return removed;
}

bool SkillLibrary::SaveLocked(std::string& outError) const
{
    if (path.empty()) return true;
    json list = json::array();
    for (const Procedure& procedure : procedures)
    {
        list.push_back({
            {"id", procedure.id},
            {"title", procedure.title},
            {"steps", procedure.steps},
            {"successes", procedure.successes},
            {"failures", procedure.failures},
            {"created_ms", ToMilliseconds(procedure.createdAt)},
            {"used_ms", ToMilliseconds(procedure.lastUsedAt)}});
    }
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::trunc);
        stream << json{{"version", 1}, {"next_id", nextId}, {"procedures", list}}.dump(2);
        stream.flush();
        if (!stream.good())
        {
            outError = "The procedure library could not be saved.";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        outError = "The procedure library could not be saved.";
        return false;
    }
    return true;
}

} // namespace revia::learning
