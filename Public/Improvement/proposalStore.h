#pragma once

#include "Improvement/codeProposal.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace revia::improvement
{

// Every proposal she has made, what became of it, and where her reviews have got to.
//
// One folder, readable without Revia: per proposal a JSON record, a Markdown write-up,
// and a .patch that `git apply` takes from the repository root. The verdicts are the
// point -- an accepted or rejected proposal, with the person's reason, is what the next
// review is shown, which is how her reviewing improves rather than only her code.
class ProposalStore
{
public:
    bool Initialize(const std::filesystem::path& directory, std::string& outError);
    [[nodiscard]] const std::filesystem::path& Directory() const { return directory; }

    // Assigns the id when it is empty. Writes the record, the write-up, and the patch.
    bool Save(CodeProposal& proposal, const std::string& diff, std::string& outError);

    // Newest first.
    [[nodiscard]] std::vector<CodeProposal> All() const;
    // By id, with or without its leading zeros: "7" finds "0007".
    [[nodiscard]] std::optional<CodeProposal> Find(const std::string& id) const;

    // The person's verdict. Only Accepted or Rejected; anything else is refused.
    bool Decide(const std::string& id, ProposalStatus verdict, const std::string& feedback,
        std::string& outError);

    // Whether this exact edit was proposed before, whatever became of it.
    [[nodiscard]] bool Known(const std::string& fingerprint) const;
    // Proven and waiting for the person. New work stops while too many are.
    [[nodiscard]] std::size_t AwaitingDecision() const;
    // Decided proposals as one line each, newest first, for the review prompt.
    [[nodiscard]] std::vector<std::string> Lessons(std::size_t maximum) const;

    [[nodiscard]] bool TaskReviewed(const std::string& taskId) const;
    void MarkTaskReviewed(const std::string& taskId);

    // Exploration walks each file in windows and moves on to whichever file was looked at
    // longest ago. Persisted, so a restart does not start every review at line 1.
    [[nodiscard]] std::size_t NextLine(const std::string& file) const;
    [[nodiscard]] std::string LeastRecentlyExplored(const std::vector<std::string>& files) const;
    void MarkExplored(const std::string& file, std::size_t nextLine, std::int64_t atEpoch);
    [[nodiscard]] std::int64_t LastExplorationEpoch() const;

    [[nodiscard]] std::filesystem::path MarkdownPath(const std::string& id) const;
    [[nodiscard]] std::filesystem::path PatchPath(const std::string& id) const;

private:
    struct Exploration
    {
        std::size_t nextLine = 1;
        std::int64_t lastEpoch = 0;
    };

    bool WriteRecordLocked(const CodeProposal& proposal, std::string& outError) const;
    bool WriteStateLocked(std::string& outError) const;
    [[nodiscard]] static std::string CanonicalId(const std::string& id);

    mutable std::mutex mutex;
    std::filesystem::path directory;
    std::vector<CodeProposal> proposals;
    std::set<std::string> reviewedTasks;
    std::map<std::string, Exploration> explored;
    std::int64_t lastExploration = 0;
};

} // namespace revia::improvement
