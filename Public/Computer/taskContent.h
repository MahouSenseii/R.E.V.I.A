#pragma once

#include <cstddef>
#include <string>

namespace revia::computer
{

// What a task is allowed to type, and where that text is permitted to come from.
//
// The defect this exists to close was not a bug in one provider. Asked to place a
// message, a 4B Main wrote "The prepared message text here" into the box and reported
// the task complete -- and it was right to, by every rule that existed: the next-step
// grammar offered a free string for `value`, nothing downstream compared what was typed
// against what the user had actually said, and the typed postcondition then confirmed
// that the invented sentence was present in the field it had just been typed into. Three
// layers agreeing on the wrong answer, because none of them held the right one.
//
// So content becomes a property of the *task*, decided by the runtime before any model
// is asked anything, and the planner is left with the two questions it is competent to
// answer: which field, and which operation.

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
    // The field the user named, when they named one -- "the Compose box", "the subject
    // line". Lower-cased words, matched loosely against a candidate's description,
    // because a user names a field the way they read it and an observer reports it the
    // way the application spells it.
    //
    // Empty means the user did not say, which is not permission to choose freely; it
    // means the destination cannot be checked and the record says so.
    std::string destination;

    // The request with the content taken out of it, for anything a model will read.
    //
    // Found by a test and worth stating plainly: holding the words in a vault achieves
    // nothing while the task description still contains them. The goal's title is the
    // user's sentence, and the decision context carries the title, so every prompt held
    // the message the payload reference existed to keep out. Redaction closes the last
    // door -- the model is told a task about content of a certain length and never the
    // content, from every direction at once.
    //
    // Empty when there was nothing to redact. The goal's own title is untouched: it is
    // the user's request in the user's own store, and rewriting it would make their
    // history less true to serve a boundary that belongs one layer out.
    std::string redacted;

    // Whether the person asked for the content to be *sent*, as opposed to placed.
    //
    // The difference between "put this in the message box" and "send this" is the
    // difference between a reversible edit and an effect that leaves the machine, and it
    // is decided here -- by reading what the person wrote -- rather than by a model
    // inferring intent from a field it happens to be looking at.
    //
    // Read conservatively, and negation is honoured: "do not send anything" sets this
    // false however many times the word appears. A parser that got this wrong in the
    // permissive direction would turn a drafting request into a sent message, so the
    // default when the request is unclear is false.
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

// The only thing a planner may write where a prepared value belongs.
//
// Not a hint and not a convention: when the runtime is holding content, the next-step
// grammar constrains `value` to this exact string, so a model physically cannot emit an
// invented sentence there. The gate substitutes the real value afterwards. Two
// independent mechanisms for one defect, because the grammar only covers the providers
// that go through it and the gate only covers the steps that reach it.
inline constexpr const char* PreparedContentToken = "<the prepared content>";

// Read the user's request. No model involved, by design.
//
// A model asked to extract the user's exact words is a model that has been shown them
// and may return an approximation -- which is the failure this whole arrangement is
// built to prevent, reintroduced one layer earlier. So this is a plain parse, and it
// recognises deliberately little:
//
//   - a quoted span is exact content. `"..."`, `'...'` and curly quotes.
//   - a drafting verb with no quoted span is a request to compose.
//   - anything else is None, and text entry is judged as it was before.
//
// Recognising little is the point. A parse that guessed which unquoted fragment was
// "the message" would sometimes place half a sentence into a box somebody then sends.
[[nodiscard]] TaskContent ExtractTaskContent(const std::string& request);

// The longest value this will take custody of. Above any message a person types by
// hand and far below anything that would be a paste of a document.
inline constexpr std::size_t MaximumTaskContentLength = 4096;

} // namespace revia::computer
