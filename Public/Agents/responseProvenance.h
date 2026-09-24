#pragma once

#include <string>

namespace revia::agents
{

// Where the words in an assistant reply came from.
//
// Revia can form durable opinions about things, and the memory classifier is what turns
// "I dislike performative politeness" into a record that she does. That only makes
// sense when the sentence was hers. Ask her to repeat "I hate jazz." and she will say
// it -- it is a reasonable thing to do when asked -- and the reply that reaches memory
// classification is indistinguishable, in text, from her volunteering the same words.
//
// So the runtime says which it was. It knows, because it read the request before the
// reply existed, and because it knows when a reply came from the reflex router rather
// than from a model at all. The classifier's instruction not to remember play-acting
// stays in the prompt, but it is defence in depth: a small model asked to notice this
// sometimes will not, and "sometimes" is not a property a durable store can be built
// on.
//
// Four classes, because four is what the runtime can actually determine. Anything
// finer -- quoted user content, autonomous opinion, a distinction between kinds of
// roleplay -- would be a label the runtime is guessing at, and a guessed provenance is
// worse than none because it looks authoritative.
enum class ResponseProvenance
{
    // Revia answered in her own voice. The only class whose opinions are hers.
    NormalGeneration,
    // The user asked her to repeat, echo, or say particular words. Whatever opinion the
    // words express belongs to whoever chose them.
    RequestedRepetition,
    // The user asked her to pretend, act as, or play a character. She is speaking as
    // someone else, on request.
    Roleplay,
    // The runtime answered without a model: a reflex, or a canned runtime message.
    // There is no view here to record, only a mechanism.
    RuntimeReflex
};

[[nodiscard]] std::string ToString(ResponseProvenance value);

// Whether a reply with this provenance can carry an opinion that is Revia's own.
//
// This is the whole point of the enum, so it is one function rather than a comparison
// repeated at every call site: a new class has to decide this question here, once.
[[nodiscard]] bool MayExpressOwnOpinion(ResponseProvenance value);

// Reads the request, not the reply.
//
// Deliberately conservative and deliberately about the *instruction*: it looks for a
// user telling Revia to speak as something other than herself. It is not a content
// filter and does not look at what she said. Talking about jazz is normal generation;
// being told to say a sentence about jazz is not.
//
// A miss costs a memory that should not have been formed and can be corrected. A false
// positive costs one opinion Revia does not record, which is recoverable by her simply
// saying it again unprompted -- so this errs toward suppressing.
[[nodiscard]] ResponseProvenance ClassifyRequestedProvenance(const std::string& userInput);

} // namespace revia::agents
