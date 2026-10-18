#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace revia::attachments
{

// Text pulled out of an attached document. Always valid UTF-8 when ok is true.
struct ExtractedText
{
    bool ok = false;
    std::string text;
    // Byte offset in `text` where each page starts, for citations. Empty when the format
    // has no pages (plain text, DOCX).
    std::vector<std::size_t> pageOffsets;
    bool truncated = false;
    // Why extraction failed, or what was left out, in words a user can act on.
    std::string reason;
};

// A text file: UTF-8 (with or without a byte-order mark) or UTF-16 with a byte-order
// mark. Refused when it holds NUL bytes or malformed sequences, which is what a binary
// file renamed to .txt looks like.
[[nodiscard]] ExtractedText DecodeTextFile(std::string_view bytes, std::size_t maxTextBytes);

// The body text of a Word document: word/document.xml from the ZIP container. The XML
// is parsed with a document type declaration refused outright and no external entity
// ever loaded, so a crafted file cannot expand entities or reach the disk or network.
[[nodiscard]] ExtractedText ExtractDocxText(std::string_view bytes, std::size_t maxTextBytes);

// The text of a PDF, page by page. Deliberately small: it reads unencrypted files whose
// content streams are uncompressed or FlateDecode (including objects packed in object
// streams), decodes text through each font's ToUnicode map when there is one and as
// WinAnsi otherwise. Encrypted files, scanned pages with no text layer and fonts with
// no usable mapping are reported, not guessed at. Every count and size is bounded.
[[nodiscard]] ExtractedText ExtractPdfText(std::string_view bytes, std::size_t maxTextBytes);

} // namespace revia::attachments
