#include "Improvement/proposalStore.h"
#include "Core/utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace revia::improvement
{

namespace
{

using json = nlohmann::json;

// Written beside the target and renamed over it, so a crash mid-write leaves the old
// file rather than half of a new one.
bool WriteWhole(const std::filesystem::path& target, const std::string& content,
    std::string& outError)
{
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    const std::filesystem::path pending = target.string() + ".pending";
    {
        std::ofstream file(pending, std::ios::binary | std::ios::trunc);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!file.good())
        {
            outError = "Could not write " + target.filename().string() + ".";
            return false;
        }
    }
    std::filesystem::rename(pending, target, error);
    if (error)
    {
        std::filesystem::remove(pending, error);
        outError = "Could not replace " + target.filename().string() + ".";
        return false;
    }
    return true;
}

std::string OneLine(std::string text, const std::size_t limit)
{
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    if (text.size() > limit) text = utf8::Prefix(text, limit) + "...";
    return text;
}

} // namespace

std::string ProposalStore::CanonicalId(const std::string& id)
{
    std::string digits;
    for (const unsigned char character : id)
    {
        if (std::isdigit(character) == 0) return id;
        digits.push_back(static_cast<char>(character));
    }
    if (digits.empty()) return id;
    const unsigned long value = std::stoul(digits.size() > 9 ? digits.substr(digits.size() - 9) : digits);
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%04lu", value);
    return buffer;
}

bool ProposalStore::Initialize(const std::filesystem::path& inputDirectory, std::string& outError)
{
    std::lock_guard lock(mutex);
    directory = inputDirectory;
    proposals.clear();
    reviewedTasks.clear();
    explored.clear();
    lastExploration = 0;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        outError = "The proposals folder could not be created: " + error.message();
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".json" ||
            entry.path().filename() == "review_state.json")
        {
            continue;
        }
        try
        {
            std::ifstream file(entry.path());
            json data;
            file >> data;
            if (auto proposal = ProposalFromJson(data)) proposals.push_back(std::move(*proposal));
        }
        catch (const std::exception&)
        {
            // One unreadable record must not hide the others.
        }
    }
    std::sort(proposals.begin(), proposals.end(), [](const auto& left, const auto& right)
    {
        return left.id > right.id;
    });

    const std::filesystem::path statePath = directory / "review_state.json";
    if (std::filesystem::is_regular_file(statePath, error))
    {
        try
        {
            std::ifstream file(statePath);
            json state;
            file >> state;
            for (const auto& task : state.value("reviewedTasks", json::array()))
                if (task.is_string()) reviewedTasks.insert(task.get<std::string>());
            // Named first: items() over the temporary value() returns would outlive it.
            const json exploredState = state.value("explored", json::object());
            for (const auto& [file, progress] : exploredState.items())
            {
                Exploration record;
                record.nextLine = progress.value("nextLine", std::size_t{1});
                record.lastEpoch = progress.value("lastEpoch", std::int64_t{0});
                explored[file] = record;
            }
            lastExploration = state.value("lastExploration", std::int64_t{0});
        }
        catch (const std::exception&)
        {
            // Lost progress only means reviewing a window again.
        }
    }
    outError.clear();
    return true;
}

bool ProposalStore::WriteRecordLocked(const CodeProposal& proposal, std::string& outError) const
{
    return WriteWhole(directory / (proposal.id + ".json"), ToJson(proposal).dump(2), outError);
}

bool ProposalStore::WriteStateLocked(std::string& outError) const
{
    json state = {
        {"reviewedTasks", json::array()},
        {"explored", json::object()},
        {"lastExploration", lastExploration}};
    for (const std::string& task : reviewedTasks) state["reviewedTasks"].push_back(task);
    for (const auto& [file, progress] : explored)
        state["explored"][file] = {{"nextLine", progress.nextLine}, {"lastEpoch", progress.lastEpoch}};
    return WriteWhole(directory / "review_state.json", state.dump(2), outError);
}

bool ProposalStore::Save(CodeProposal& proposal, const std::string& diff, std::string& outError)
{
    std::lock_guard lock(mutex);
    if (directory.empty())
    {
        outError = "The proposal store is not initialised.";
        return false;
    }
    if (proposal.id.empty())
    {
        unsigned long highest = 0;
        for (const CodeProposal& existing : proposals)
        {
            try { highest = std::max(highest, std::stoul(existing.id)); }
            catch (...) {}
        }
        proposal.id = CanonicalId(std::to_string(highest + 1));
    }
    if (proposal.fingerprint.empty()) proposal.fingerprint = Fingerprint(proposal.change);
    if (!WriteRecordLocked(proposal, outError)) return false;
    // The write-up and the patch are conveniences; the record is what matters, so their
    // failure is reported but does not undo it.
    std::string ignored;
    (void)WriteWhole(directory / (proposal.id + ".md"), ToMarkdown(proposal, diff), ignored);
    if (!diff.empty()) (void)WriteWhole(directory / (proposal.id + ".patch"), diff, ignored);

    const auto existing = std::find_if(proposals.begin(), proposals.end(),
        [&proposal](const CodeProposal& other) { return other.id == proposal.id; });
    if (existing != proposals.end()) *existing = proposal;
    else proposals.insert(proposals.begin(), proposal);
    outError.clear();
    return true;
}

std::vector<CodeProposal> ProposalStore::All() const
{
    std::lock_guard lock(mutex);
    return proposals;
}

std::optional<CodeProposal> ProposalStore::Find(const std::string& id) const
{
    std::lock_guard lock(mutex);
    const std::string wanted = CanonicalId(id);
    for (const CodeProposal& proposal : proposals)
        if (proposal.id == wanted) return proposal;
    return std::nullopt;
}

bool ProposalStore::Decide(
    const std::string& id, const ProposalStatus verdict, const std::string& feedback,
    std::string& outError)
{
    if (verdict != ProposalStatus::Accepted && verdict != ProposalStatus::Rejected)
    {
        outError = "A proposal can only be accepted or rejected.";
        return false;
    }
    std::lock_guard lock(mutex);
    const std::string wanted = CanonicalId(id);
    for (CodeProposal& proposal : proposals)
    {
        if (proposal.id != wanted) continue;
        // Written before it is believed: a verdict that failed to reach disk must not
        // stand in memory, where the next review would learn from it until a restart.
        CodeProposal decided = proposal;
        decided.status = verdict;
        decided.feedback = feedback;
        if (!WriteRecordLocked(decided, outError)) return false;
        proposal = std::move(decided);
        // The write-up carries the verdict too, so the folder tells the whole story.
        std::string diff;
        std::ifstream patch(directory / (proposal.id + ".patch"), std::ios::binary);
        if (patch)
        {
            std::ostringstream buffer;
            buffer << patch.rdbuf();
            diff = buffer.str();
        }
        std::string ignored;
        (void)WriteWhole(directory / (proposal.id + ".md"), ToMarkdown(proposal, diff), ignored);
        outError.clear();
        return true;
    }
    outError = "There is no proposal " + id + ".";
    return false;
}

bool ProposalStore::Known(const std::string& fingerprint) const
{
    std::lock_guard lock(mutex);
    return std::any_of(proposals.begin(), proposals.end(),
        [&fingerprint](const CodeProposal& proposal) { return proposal.fingerprint == fingerprint; });
}

std::size_t ProposalStore::AwaitingDecision() const
{
    std::lock_guard lock(mutex);
    return static_cast<std::size_t>(std::count_if(proposals.begin(), proposals.end(),
        [](const CodeProposal& proposal) { return proposal.status == ProposalStatus::Verified; }));
}

std::vector<std::string> ProposalStore::Lessons(const std::size_t maximum) const
{
    std::lock_guard lock(mutex);
    std::vector<std::string> lessons;
    for (const CodeProposal& proposal : proposals)
    {
        if (lessons.size() >= maximum) break;
        if (proposal.status == ProposalStatus::Accepted)
        {
            lessons.push_back("ACCEPTED \"" + OneLine(proposal.title, 100) + "\" in " +
                proposal.change.path + (proposal.feedback.empty() ? std::string{}
                    : ". Their words: " + OneLine(proposal.feedback, 200)));
        }
        else if (proposal.status == ProposalStatus::Rejected)
        {
            lessons.push_back("REJECTED \"" + OneLine(proposal.title, 100) + "\" in " +
                proposal.change.path + (proposal.feedback.empty() ? std::string{}
                    : ". Their reason: " + OneLine(proposal.feedback, 200)));
        }
        else if (proposal.status == ProposalStatus::FailedVerification)
        {
            lessons.push_back("FAILED TO BUILD OR BROKE TESTS \"" + OneLine(proposal.title, 100) +
                "\" in " + proposal.change.path + ": " +
                OneLine(proposal.verificationSummary, 200));
        }
    }
    return lessons;
}

bool ProposalStore::TaskReviewed(const std::string& taskId) const
{
    std::lock_guard lock(mutex);
    return reviewedTasks.count(taskId) != 0;
}

void ProposalStore::MarkTaskReviewed(const std::string& taskId)
{
    std::lock_guard lock(mutex);
    reviewedTasks.insert(taskId);
    std::string ignored;
    (void)WriteStateLocked(ignored);
}

std::size_t ProposalStore::NextLine(const std::string& file) const
{
    std::lock_guard lock(mutex);
    const auto found = explored.find(file);
    return found == explored.end() ? 1 : std::max<std::size_t>(1, found->second.nextLine);
}

std::string ProposalStore::LeastRecentlyExplored(const std::vector<std::string>& files) const
{
    std::lock_guard lock(mutex);
    std::string chosen;
    std::int64_t oldest = 0;
    bool first = true;
    for (const std::string& file : files)
    {
        const auto found = explored.find(file);
        const std::int64_t when = found == explored.end() ? 0 : found->second.lastEpoch;
        if (first || when < oldest)
        {
            chosen = file;
            oldest = when;
            first = false;
        }
    }
    return chosen;
}

void ProposalStore::MarkExplored(
    const std::string& file, const std::size_t nextLine, const std::int64_t atEpoch)
{
    std::lock_guard lock(mutex);
    explored[file] = {nextLine, atEpoch};
    lastExploration = atEpoch;
    std::string ignored;
    (void)WriteStateLocked(ignored);
}

std::int64_t ProposalStore::LastExplorationEpoch() const
{
    std::lock_guard lock(mutex);
    return lastExploration;
}

std::filesystem::path ProposalStore::MarkdownPath(const std::string& id) const
{
    return directory / (CanonicalId(id) + ".md");
}

std::filesystem::path ProposalStore::PatchPath(const std::string& id) const
{
    return directory / (CanonicalId(id) + ".patch");
}

} // namespace revia::improvement
