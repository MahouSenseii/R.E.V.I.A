#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace revia::attachments
{

struct DocumentChunk
{
    std::size_t index = 0;
    std::string text;
    // 1-based page the chunk starts on, or 0 when the document has no pages.
    std::size_t page = 0;
};

// Sizes are in bytes. At the roughly four bytes per token of English prose the defaults
// give chunks of about 800 to 1,200 tokens with about 100 tokens of overlap, and the hard
// ceiling keeps every chunk inside the embedding server's 2,048-token batch even for
// denser text.
struct ChunkingOptions
{
    std::size_t targetBytes = 3600;
    std::size_t overlapBytes = 400;
    std::size_t maxBytes = 4800;
    std::size_t maxChunks = 2000;
};

// Splits `text` at paragraph, line, sentence or word boundaries near the target size,
// never inside a UTF-8 sequence, with each chunk repeating the end of the one before so
// a sentence on a boundary is still found whole in one of them.
[[nodiscard]] std::vector<DocumentChunk> ChunkDocument(
    const std::string& text, const std::vector<std::size_t>& pageOffsets, const ChunkingOptions& options = {});

} // namespace revia::attachments
