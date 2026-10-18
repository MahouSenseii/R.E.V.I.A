#include "Attachments/documentChunker.h"

#include <algorithm>
#include <string_view>

namespace revia::attachments
{
namespace
{
bool IsContinuation(const char character)
{
    return (static_cast<unsigned char>(character) & 0xC0U) == 0x80U;
}

// Moves `at` back to the start of a UTF-8 sequence.
std::size_t CharacterBoundary(const std::string& text, std::size_t at)
{
    while (at > 0 && at < text.size() && IsContinuation(text[at]))
        --at;
    return at;
}

// The best place to end a chunk in [earliest, latest]: the last paragraph break, then
// line break, then sentence end, then space. Falls back to a hard cut at `latest`.
std::size_t BreakPoint(const std::string& text, const std::size_t earliest, const std::size_t latest)
{
    const std::string_view window(text.data() + earliest, latest - earliest);
    for (const std::string_view separator : {std::string_view("\n\n"), std::string_view("\n"), std::string_view(". "),
             std::string_view("? "), std::string_view("! "), std::string_view(" ")})
    {
        const std::size_t found = window.rfind(separator);
        if (found != std::string_view::npos && found > 0)
            return earliest + found + separator.size();
    }
    return CharacterBoundary(text, latest);
}

std::size_t PageAt(const std::vector<std::size_t>& pageOffsets, const std::size_t offset)
{
    if (pageOffsets.empty())
        return 0;
    const auto after = std::upper_bound(pageOffsets.begin(), pageOffsets.end(), offset);
    return static_cast<std::size_t>(std::max<std::ptrdiff_t>(1, after - pageOffsets.begin()));
}

bool IsBlank(const std::string_view text)
{
    return std::all_of(text.begin(), text.end(), [](const char character) { return character == ' ' || character == '\n' || character == '\t'; });
}
} // namespace

std::vector<DocumentChunk> ChunkDocument(const std::string& text, const std::vector<std::size_t>& pageOffsets, const ChunkingOptions& options)
{
    std::vector<DocumentChunk> chunks;
    const std::size_t target = std::max<std::size_t>(64, options.targetBytes);
    const std::size_t maximum = std::max(target, options.maxBytes);
    const std::size_t overlap = std::min(options.overlapBytes, target / 2);
    std::size_t start = 0;
    while (start < text.size() && chunks.size() < options.maxChunks)
    {
        // The rest becomes one chunk when it is within a quarter of the target; otherwise
        // the chunk ends at the best boundary past 60% of the target, never beyond the
        // hard ceiling, so chunks come out neither tiny nor oversized.
        const std::size_t window = std::min(maximum, target + target / 4);
        std::size_t end = text.size();
        if (text.size() - start > window)
            end = BreakPoint(text, start + target * 6 / 10, start + window);
        if (end <= start)
            end = CharacterBoundary(text, std::min(text.size(), start + maximum));
        if (end <= start)
            end = text.size();

        const std::string_view piece(text.data() + start, end - start);
        if (!IsBlank(piece))
        {
            // Leading whitespace carried in by the overlap is not part of the content.
            const std::size_t skip = piece.find_first_not_of(" \n\t");
            DocumentChunk chunk;
            chunk.index = chunks.size();
            chunk.text.assign(piece.substr(skip));
            while (!chunk.text.empty() && (chunk.text.back() == ' ' || chunk.text.back() == '\n' || chunk.text.back() == '\t'))
                chunk.text.pop_back();
            chunk.page = PageAt(pageOffsets, start + skip);
            chunks.push_back(std::move(chunk));
        }
        if (end >= text.size())
            break;

        // Step back by the overlap, then forward to a word start so the repeated part
        // begins cleanly. Always advance, so the loop ends.
        std::size_t next = end > overlap ? end - overlap : end;
        if (next <= start)
            next = end;
        const std::size_t space = text.find_first_of(" \n", next);
        if (space != std::string::npos && space < end)
            next = space + 1;
        next = CharacterBoundary(text, next);
        start = next > start ? next : end;
    }
    return chunks;
}

} // namespace revia::attachments
