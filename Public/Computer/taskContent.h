#pragma once

#include <cstddef>
#include <string>

namespace revia::computer
{

// Runtime extracts exact task content before planning; models choose only
// the destination and operation.

// Where a value came from. Carried with the value for its whole life, because "the user
// wrote this" and "she composed this" are different things to place, to verify and to
// show in a record -- and a value that lost the distinction would be indistinguishable
// from one a planner made up.
enum class ContentProvenance
{
    None,
    // The user's own words, lifted from their request by a deterministic parse. Never
    // regenerated, never paraphrased, never shown to a model.
    UserSupplied,
    // Composed, because composing was the thing that was asked for. Legitimate, and
    // still fixed at the moment it is chosen: a draft that changed between the attempt
    // that typed it and the check that read it back would verify against itself.
    Drafted
};

[[nodiscard]] std::string ToString(ContentProvenance value);

// What the task needs typed, as a question about the request rather than about a step.
enum class ContentRequirement
{
    // Nothing identifiable. Text entry is judged exactly as it was before this existed:
    // a URL, a search term or a filename the planner composes from the request is
    // ordinary work, and refusing all of it would break tasks that never had a payload.
    None,
    // The user supplied the words. Exactly those words are placed, whatever any planner
    // proposes, and a step that cannot place them fails rather than placing something
    // else.
    ExactUserContent,
    // The user asked for something to be composed. The first value chosen for entry is
    // taken into custody and becomes the task's value; later steps place that one.
    Drafted
};

[[nodiscard]] std::string ToString(ContentRequirement value);

// What the runtime extracted from the user's own request, before a model saw it.
struct TaskContent
{
    ContentRequirement requirement = ContentRequirement::None;
    // Exact, and populated only for ExactUserContent. Empty for Drafted, where the
    // value does not exist yet.
    std::string value;
    // A coarse label a policy may reason about without being shown the value.
    std::string kind;
    // User-named field, normalized for descriptor matching. Empty means the
    // destination is unverified, not permission to choose freely.
    std::string destination;

    // Content-free request for model prompts; empty when no redaction was needed.
    // The stored goal title retains the user's original request.
    std::string redacted;

    // True only for an explicit request to submit; negation or uncertainty keeps it false.
    // Placing content alone never authorizes sending.
    bool submissionRequested = false;

    // What the person called the thing that submits, when they named it. "send",
    // "post", "reply". Used to look for exactly one such control; a task that names
    // none, or names one that matches two controls, is a question for a person.
    std::string submissionVerb;

    [[nodiscard]] bool RequiresExactContent() const
    {
        return requirement == ContentRequirement::ExactUserContent;
    }
    [[nodiscard]] bool Any() const { return requirement != ContentRequirement::None; }
};

// Planner grammar requires this token for held content; the gate supplies the original.
inline constexpr const char* PreparedContentToken = "<the prepared content>";

// Parses exact quoted text, or a drafting request without quotes.
// Other input is None; never guesses content from an unquoted fragment.
[[nodiscard]] TaskContent ExtractTaskContent(const std::string& request);

// The longest value this will take custody of. Above any message a person types by
// hand and far below anything that would be a paste of a document.
inline constexpr std::size_t MaximumTaskContentLength = 4096;

} // namespace revia::computer
