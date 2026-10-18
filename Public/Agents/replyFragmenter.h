#pragma once

#include <string>
#include <vector>

namespace revia::agents
{

// Emits complete sentences during streaming; legacy character targets remain compatible.
// Boundaries require terminal punctuation and whitespace/end, excluding decimals, abbreviations and ellipses.
class ReplyFragmenter
{
public:
    explicit ReplyFragmenter(std::size_t minimumFragmentCharacters = 24, std::size_t maximumPhraseCharacters = 0,
        std::size_t firstMinimumFragmentCharacters = 0, std::size_t firstMaximumPhraseCharacters = 0);

    // Feeds newly generated text. Returns any fragments that are now complete.
    [[nodiscard]] std::vector<std::string> Consume(const std::string& incoming);
    // Feeds text the caller knows ends a sentence, so its last sentence can be emitted
    // without waiting for the whitespace that would otherwise confirm the boundary.
    [[nodiscard]] std::vector<std::string> ConsumeSentences(const std::string& incoming);
    // Whatever is left when generation ends, if it is worth speaking.
    [[nodiscard]] std::string Flush();
    // Text received but not emitted yet. Flush may judge it not worth speaking; it is
    // still part of the reply.
    [[nodiscard]] std::string Pending() const;
    void Reset();

    [[nodiscard]] static bool IsBoundary(const std::string& text, std::size_t index);
    // Where the sentence whose terminal punctuation is at `index` ends, past any closing
    // quote or bracket; npos when `index` is not a confirmed boundary.
    [[nodiscard]] static std::size_t SentenceEnd(const std::string& text, std::size_t index);

private:
    std::string pending;
    std::size_t followingMinimumCharacters;
    std::size_t followingMaximumCharacters;
    std::size_t firstMinimumCharacters;
    std::size_t firstMaximumCharacters;
    std::size_t emittedFragments = 0;
};

} // namespace revia::agents
