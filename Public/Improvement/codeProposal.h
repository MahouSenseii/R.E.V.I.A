#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace revia::improvement
{

// Revia reviewing her own code.
//
// She reads a bounded window of her source, suggests at most one concrete change with the
// problem and the reason, and proves it in a separate copy of the tree -- it builds and
// the test suites still pass -- before anyone is told. She never edits the real source:
// the change reaches it only when a person applies the patch. What makes this
// self-improvement rather than a linter is the loop around it: what she looked at comes
// from evidence about her own behaviour, and every verdict on a proposal is fed back into
// the next review.

enum class ProposalStatus
{
    // Written, and passed the deterministic checks; not yet built.
    Drafted,
    // Built and tested in the workbench, with no test that passed before failing after.
    Verified,
    // Did not build, or broke a test that passed without it.
    FailedVerification,
    // A person read it and agreed.
    Accepted,
    // A person read it and said no, with a reason she learns from.
    Rejected
};

[[nodiscard]] std::string ToString(ProposalStatus status);
[[nodiscard]] std::optional<ProposalStatus> ProposalStatusFromString(const std::string& text);

// One edit, as text to find and text to put in its place. Not a diff: a small model
// copies a snippet far more reliably than it counts lines, and "occurs exactly once" is a
// check a diff's line numbers cannot offer.
struct CodeChange
{
    // Forward slashes, relative to the source root.
    std::string path;
    std::string find;
    std::string replace;
};

struct CodeProposal
{
    std::string id;
    // Unix seconds, as text so the record reads the same everywhere.
    std::string createdAt;
    // "evidence", "exploration", or "request": why she was looking at all.
    std::string trigger;
    // The self-assessment task that pointed her here, when one did.
    std::string taskId;

    std::string title;
    // What is wrong, in the code as it stands.
    std::string problem;
    // Why the change fixes it -- the part a person needs to judge it.
    std::string reason;
    // What supports it: the observed symptom, or the lines that show the defect.
    std::string evidence;
    std::string expectedBenefit;
    std::string risks;
    // 0..1, her own estimates. Advisory; the thresholds that act on them are deterministic.
    double benefit = 0.0;
    double risk = 0.0;
    CodeChange change;

    ProposalStatus status = ProposalStatus::Drafted;
    // One line: "builds; 10/10 test suites pass" or what broke.
    std::string verificationSummary;
    // The person's words on accepting or rejecting it.
    std::string feedback;
    // Stable identity of the edit itself, so a change already proposed -- in any status
    // -- is never proposed again.
    std::string fingerprint;
};

[[nodiscard]] nlohmann::json ToJson(const CodeProposal& proposal);
[[nodiscard]] std::optional<CodeProposal> ProposalFromJson(const nlohmann::json& data);

// Hex hash of path, find, and replace with line endings and trailing space normalised.
[[nodiscard]] std::string Fingerprint(const CodeChange& change);

// CRLF to LF. Every comparison of model text against source goes through this.
[[nodiscard]] std::string NormalizeLineEndings(std::string text);

// A model's review reply, parsed and bounded. Empty when she found nothing -- the normal,
// correct outcome of most reviews -- with the reason in outNote either way.
[[nodiscard]] std::optional<CodeProposal> ParseReviewReply(
    const std::string& raw, std::string& outNote);

struct ChangeCheck
{
    bool ok = false;
    std::string reason;
    // The text to replace is not in the file exactly once.
    bool notFound = false;
};

// Everything that can be decided about a change without compiling it.
//
// The find text must occur exactly once in the current file, the change must be small,
// and it must not introduce anything that reaches outside the process -- starting
// programs, the network, deleting files, the registry. "Introduce" matters: code that
// already launches the voice worker may be edited, but a change may not add a launch
// that was not there. Proving a change means compiling and running it, so this is the
// gate in front of executing model-written code.
[[nodiscard]] ChangeCheck CheckChange(
    const CodeChange& change, const std::string& currentContent);

// The first line of `find` that appears nowhere in the file, ignoring indentation: where
// a copy that did not match went wrong. Empty when every line is somewhere in the file,
// which means the lines are real but not together, or together more than once.
[[nodiscard]] std::string FirstLineNotInFile(
    const std::string& currentContent, const std::string& find);

// Whether two changes to the same file edit any of the same lines of it.
[[nodiscard]] bool Overlaps(
    const std::string& currentContent, const CodeChange& first, const CodeChange& second);

// The file with the change applied. Empty optional when the find text is not there
// exactly once.
[[nodiscard]] std::optional<std::string> ApplyChange(
    const std::string& currentContent, const CodeChange& change);

// A unified diff, three lines of context, that `git apply` accepts from the repository
// root.
[[nodiscard]] std::string MakeUnifiedDiff(
    const std::string& currentContent, const CodeChange& change);

// The human-readable record: what, why, evidence, verification, and the diff.
[[nodiscard]] std::string ToMarkdown(const CodeProposal& proposal, const std::string& diff);

} // namespace revia::improvement
