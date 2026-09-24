#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace revia::content
{

// One addressable unit of the document: a paragraph, a line of dialogue, a stage
// direction. Ids are stable across edits so a reference survives insertions above it.
struct Block
{
    std::string id;
    std::string text;
    // 1-based position, recomputed on every change. What the user types when they say
    // "change line 4"; the id is what the code uses.
    int ordinal = 0;
};

struct EditOutcome
{
    bool succeeded = false;
    std::string message;
    std::string blockId;
    std::string before;
    std::string after;
};

// A document Revia is working on, held in the session.
//
// The point of this type is the shape of its mutations. A precise edit is not a request
// to a model to please leave the rest alone -- it is `ReplaceBlock`, which can reach
// exactly one block and has no expression for touching another. Asking a model to rewrite
// a scene and preserve everything but one line works until the day it does not, and the
// failure is silent: the scene still reads fine, and a paragraph three pages up has
// quietly changed. Here that outcome is unreachable rather than unlikely.
//
// Everything else follows from that: generation replaces the whole document explicitly,
// edits are per-block, and every mutation snapshots first so any of it can be undone.
class WorkingDocument
{
public:
    // Beyond this the buffer stops being a working document and starts being a file.
    static constexpr std::size_t MaximumBlocks = 400;
    static constexpr std::size_t MaximumBlockCharacters = 8000;
    static constexpr std::size_t MaximumRevisions = 50;
    // Undo depth has to be bounded by size, not only by count. Fifty snapshots of a
    // document at its own ceilings is 400 blocks times 8 KB times 50 -- about 160 MB of
    // history behind a buffer the user thinks of as a page of text. Counting characters
    // keeps deep undo on small documents and stops large ones from hoarding.
    static constexpr std::size_t MaximumHistoryCharacters = 4 * 1024 * 1024;

    void SetTitle(std::string value);
    [[nodiscard]] const std::string& Title() const;
    [[nodiscard]] const std::vector<Block>& Blocks() const;
    [[nodiscard]] bool IsEmpty() const;
    [[nodiscard]] std::size_t RevisionCount() const;

    // Accepts an ordinal ("4") or a block id ("b4"), because the user reads ordinals off
    // the screen and the code passes ids around.
    [[nodiscard]] const Block* Find(const std::string& reference) const;

    // Replaces the whole document. Generation is honest about being wholesale rather than
    // pretending to be an edit.
    void Compose(std::string title, std::vector<std::string> blocks);
    EditOutcome Append(std::string text);

    // The precise edit. One block, by construction.
    EditOutcome ReplaceBlock(const std::string& reference, std::string text);
    EditOutcome InsertAfter(const std::string& reference, std::string text);
    EditOutcome RemoveBlock(const std::string& reference);

    // Returns false when there is nothing left to undo.
    bool Undo();
    void Clear();

    // The document as text, ready to save or paste.
    [[nodiscard]] std::string Render() const;
    // The document with ordinals, ready to read on screen and refer to.
    [[nodiscard]] std::string RenderNumbered() const;
    // The blocks around a target, for giving a model enough context to rewrite one line
    // without handing it the whole document to rewrite.
    [[nodiscard]] std::string RenderNeighbourhood(
        const std::string& reference,
        std::size_t radius = 2) const;

private:
    void Snapshot();
    void Renumber();
    [[nodiscard]] std::vector<Block>::iterator Locate(const std::string& reference);

    std::string title;
    std::vector<Block> blocks;
    std::vector<std::vector<Block>> history;
    std::vector<std::string> historyTitles;
    std::vector<std::size_t> historySizes;
    std::size_t historyCharacters = 0;
    std::size_t nextBlockId = 1;
};

// Catches the failure the block model is designed to prevent, in the one place it can
// still get in: a model that was asked for one replacement line and returned the whole
// scene. Storing that as a single block would not corrupt the others, but it would
// collapse the document into one paragraph, which is its own kind of broken.
class PreciseEditGuard
{
public:
    // True when the replacement has evidently swallowed neighbouring blocks.
    [[nodiscard]] static bool LooksLikeWholeDocument(
        const std::string& replacement,
        const std::vector<Block>& others,
        const std::string& targetId);

    // Strips a model's habitual framing -- "Sure, here's the revised line:", quotes,
    // a code fence -- so the block holds the line and not the apology in front of it.
    [[nodiscard]] static std::string CleanReplacement(const std::string& response);
};

} // namespace revia::content
