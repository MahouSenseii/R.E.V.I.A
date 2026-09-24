#pragma once

#include <string>
#include <vector>

namespace revia::agents
{

// Cuts a streaming reply into speakable pieces as the tokens arrive.
//
// Waiting for a whole reply before starting to speak wastes the entire generation time:
// the first sentence is usually complete long before the last one exists. Emitting on
// sentence boundaries lets speech start on sentence one while the rest is still being
// produced, so the reply begins in about the time it takes to generate a sentence rather
// than a paragraph. Legacy character targets remain accepted for configuration
// compatibility, but only complete sentence boundaries may create spoken jobs.
//
// The cost of getting this wrong is speech that stops mid-clause, so a boundary is only
// accepted when it is unambiguous: terminal punctuation followed by whitespace or the end
// of the stream, and never inside a decimal, an abbreviation, or an ellipsis.
class ReplyFragmenter
{
public:
    explicit ReplyFragmenter(
        std::size_t minimumFragmentCharacters = 24,
        std::size_t maximumPhraseCharacters = 0,
        std::size_t firstMinimumFragmentCharacters = 0,
        std::size_t firstMaximumPhraseCharacters = 0);

    // Feeds newly generated text. Returns any fragments that are now complete.
    [[nodiscard]] std::vector<std::string> Consume(const std::string& incoming);
    // Whatever is left when generation ends, if it is worth speaking.
    [[nodiscard]] std::string Flush();
    void Reset();

    [[nodiscard]] static bool IsBoundary(const std::string& text, std::size_t index);

private:
    std::string pending;
    std::size_t followingMinimumCharacters;
    std::size_t followingMaximumCharacters;
    std::size_t firstMinimumCharacters;
    std::size_t firstMaximumCharacters;
    std::size_t emittedFragments = 0;
};

} // namespace revia::agents
