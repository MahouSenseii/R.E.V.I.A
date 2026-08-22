#include "Content/workingDocument.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace revia::content
{

namespace
{

std::string Trim(std::string value)
{
    const auto notSpace = [](const unsigned char character)
    {
        return !std::isspace(character);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string Clamp(std::string text)
{
    text = Trim(std::move(text));
    if (text.size() > WorkingDocument::MaximumBlockCharacters)
    {
        text.resize(WorkingDocument::MaximumBlockCharacters);
    }
    return text;
}

} // namespace

void WorkingDocument::SetTitle(std::string value)
{
    title = Trim(std::move(value));
}

const std::string& WorkingDocument::Title() const
{
    return title;
}

const std::vector<Block>& WorkingDocument::Blocks() const
{
    return blocks;
}

bool WorkingDocument::IsEmpty() const
{
    return blocks.empty();
}

std::size_t WorkingDocument::RevisionCount() const
{
    return history.size();
}

void WorkingDocument::Renumber()
{
    int ordinal = 1;
    for (Block& block : blocks)
    {
        block.ordinal = ordinal++;
    }
}

void WorkingDocument::Snapshot()
{
    std::size_t size = title.size();
    for (const Block& block : blocks)
    {
        size += block.text.size() + block.id.size();
    }

    history.push_back(blocks);
    historyTitles.push_back(title);
    historySizes.push_back(size);
    historyCharacters += size;

    // Bounded by both, and the oldest goes first either way. At least one revision is
    // always kept, so undo never becomes unavailable just because one block is enormous.
    while (history.size() > 1 &&
        (history.size() > MaximumRevisions ||
            historyCharacters > MaximumHistoryCharacters))
    {
        historyCharacters -= historySizes.front();
        history.erase(history.begin());
        historyTitles.erase(historyTitles.begin());
        historySizes.erase(historySizes.begin());
    }
}

std::vector<Block>::iterator WorkingDocument::Locate(const std::string& reference)
{
    const std::string wanted = Trim(reference);
    if (wanted.empty())
    {
        return blocks.end();
    }
    const auto byId = std::find_if(blocks.begin(), blocks.end(),
        [&wanted](const Block& block) { return block.id == wanted; });
    if (byId != blocks.end())
    {
        return byId;
    }
    // An ordinal is what the user reads off the screen, so it has to work as a reference.
    const bool numeric = std::all_of(wanted.begin(), wanted.end(),
        [](const unsigned char character) { return std::isdigit(character) != 0; });
    if (!numeric)
    {
        return blocks.end();
    }
    const int ordinal = std::stoi(wanted);
    return std::find_if(blocks.begin(), blocks.end(),
        [ordinal](const Block& block) { return block.ordinal == ordinal; });
}

const Block* WorkingDocument::Find(const std::string& reference) const
{
    // Const lookup shares the mutable one's rules rather than keeping a second copy of
    // them, because two lookups that disagree about what "4" means is a real bug.
    auto& mutableSelf = const_cast<WorkingDocument&>(*this);
    const auto found = mutableSelf.Locate(reference);
    return found == mutableSelf.blocks.end() ? nullptr : &*found;
}

void WorkingDocument::Compose(std::string newTitle, std::vector<std::string> newBlocks)
{
    Snapshot();
    title = Trim(std::move(newTitle));
    blocks.clear();
    for (std::string& text : newBlocks)
    {
        if (blocks.size() >= MaximumBlocks)
        {
            break;
        }
        std::string clamped = Clamp(std::move(text));
        if (clamped.empty())
        {
            continue;
        }
        blocks.push_back({"b" + std::to_string(nextBlockId++), std::move(clamped), 0});
    }
    Renumber();
}

EditOutcome WorkingDocument::Append(std::string text)
{
    EditOutcome outcome;
    std::string clamped = Clamp(std::move(text));
    if (clamped.empty())
    {
        outcome.message = "There was nothing to add.";
        return outcome;
    }
    if (blocks.size() >= MaximumBlocks)
    {
        outcome.message = "The document is at its " + std::to_string(MaximumBlocks) +
            "-block ceiling.";
        return outcome;
    }
    Snapshot();
    blocks.push_back({"b" + std::to_string(nextBlockId++), std::move(clamped), 0});
    Renumber();
    outcome.succeeded = true;
    outcome.blockId = blocks.back().id;
    outcome.after = blocks.back().text;
    outcome.message = "Added block " + std::to_string(blocks.back().ordinal) + '.';
    return outcome;
}

EditOutcome WorkingDocument::ReplaceBlock(const std::string& reference, std::string text)
{
    EditOutcome outcome;
    const auto target = Locate(reference);
    if (target == blocks.end())
    {
        outcome.message = "There is no block " + reference + '.';
        return outcome;
    }
    std::string clamped = Clamp(std::move(text));
    if (clamped.empty())
    {
        outcome.message = "A replacement cannot be empty. Remove the block instead.";
        return outcome;
    }

    Snapshot();
    outcome.before = target->text;
    // The single assignment this whole type exists to constrain. Nothing else is touched
    // because nothing else is reachable from here.
    target->text = std::move(clamped);
    outcome.succeeded = true;
    outcome.blockId = target->id;
    outcome.after = target->text;
    outcome.message = "Rewrote block " + std::to_string(target->ordinal) +
        " and left the other " + std::to_string(blocks.size() - 1) +
        (blocks.size() == 2 ? " block" : " blocks") + " untouched.";
    return outcome;
}

EditOutcome WorkingDocument::InsertAfter(const std::string& reference, std::string text)
{
    EditOutcome outcome;
    const auto target = Locate(reference);
    if (target == blocks.end())
    {
        outcome.message = "There is no block " + reference + '.';
        return outcome;
    }
    std::string clamped = Clamp(std::move(text));
    if (clamped.empty())
    {
        outcome.message = "There was nothing to insert.";
        return outcome;
    }
    if (blocks.size() >= MaximumBlocks)
    {
        outcome.message = "The document is at its block ceiling.";
        return outcome;
    }

    Snapshot();
    const std::string id = "b" + std::to_string(nextBlockId++);
    const auto inserted = blocks.insert(target + 1, {id, std::move(clamped), 0});
    Renumber();
    outcome.succeeded = true;
    outcome.blockId = id;
    outcome.after = inserted->text;
    outcome.message = "Inserted a block at position " + std::to_string(inserted->ordinal) + '.';
    return outcome;
}

EditOutcome WorkingDocument::RemoveBlock(const std::string& reference)
{
    EditOutcome outcome;
    const auto target = Locate(reference);
    if (target == blocks.end())
    {
        outcome.message = "There is no block " + reference + '.';
        return outcome;
    }
    Snapshot();
    outcome.before = target->text;
    outcome.blockId = target->id;
    const int ordinal = target->ordinal;
    blocks.erase(target);
    Renumber();
    outcome.succeeded = true;
    outcome.message = "Removed block " + std::to_string(ordinal) + '.';
    return outcome;
}

bool WorkingDocument::Undo()
{
    if (history.empty())
    {
        return false;
    }
    blocks = std::move(history.back());
    title = std::move(historyTitles.back());
    historyCharacters -= historySizes.back();
    history.pop_back();
    historyTitles.pop_back();
    historySizes.pop_back();
    Renumber();
    return true;
}

void WorkingDocument::Clear()
{
    Snapshot();
    blocks.clear();
    title.clear();
}

std::string WorkingDocument::Render() const
{
    std::ostringstream stream;
    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
        if (index > 0)
        {
            stream << "\n\n";
        }
        stream << blocks[index].text;
    }
    return stream.str();
}

std::string WorkingDocument::RenderNumbered() const
{
    if (blocks.empty())
    {
        return "The working document is empty.";
    }
    std::ostringstream stream;
    if (!title.empty())
    {
        stream << title << "\n";
    }
    for (const Block& block : blocks)
    {
        stream << '\n' << block.ordinal << ". " << block.text;
    }
    return stream.str();
}

std::string WorkingDocument::RenderNeighbourhood(
    const std::string& reference,
    const std::size_t radius) const
{
    const Block* target = Find(reference);
    if (target == nullptr)
    {
        return {};
    }
    const auto position = static_cast<std::size_t>(target->ordinal - 1);
    const std::size_t first = position > radius ? position - radius : 0;
    const std::size_t last = std::min(blocks.size(), position + radius + 1);

    std::ostringstream stream;
    for (std::size_t index = first; index < last; ++index)
    {
        if (index > first)
        {
            stream << '\n';
        }
        stream << (blocks[index].id == target->id ? ">> " : "   ")
            << blocks[index].ordinal << ". " << blocks[index].text;
    }
    return stream.str();
}

bool PreciseEditGuard::LooksLikeWholeDocument(
    const std::string& replacement,
    const std::vector<Block>& others,
    const std::string& targetId)
{
    const std::string lowered = Lower(replacement);
    std::size_t swallowed = 0;
    for (const Block& block : others)
    {
        if (block.id == targetId)
        {
            continue;
        }
        // Short blocks match by coincidence; a long one appearing verbatim does not.
        const std::string other = Lower(Trim(block.text));
        if (other.size() < 24)
        {
            continue;
        }
        if (lowered.find(other) != std::string::npos)
        {
            ++swallowed;
        }
    }
    return swallowed > 0;
}

std::string PreciseEditGuard::CleanReplacement(const std::string& response)
{
    std::string cleaned = Trim(response);

    // A fenced block, with or without a language tag.
    if (cleaned.rfind("```", 0) == 0)
    {
        const std::size_t firstNewline = cleaned.find('\n');
        if (firstNewline != std::string::npos)
        {
            cleaned = cleaned.substr(firstNewline + 1);
        }
        const std::size_t closing = cleaned.rfind("```");
        if (closing != std::string::npos)
        {
            cleaned = cleaned.substr(0, closing);
        }
        cleaned = Trim(std::move(cleaned));
    }

    // "Here's the revised line:" and its relatives, when a preamble is followed by the
    // actual line on its own. Only stripped when something survives it.
    static const std::vector<std::string> preambles = {
        "here's the revised", "here is the revised", "here's the rewritten",
        "here is the rewritten", "revised line:", "rewritten line:",
        "sure, here", "sure! here", "certainly, here"
    };
    const std::string lowered = Lower(cleaned);
    for (const std::string& preamble : preambles)
    {
        if (lowered.rfind(preamble, 0) != 0)
        {
            continue;
        }
        const std::size_t newline = cleaned.find('\n');
        if (newline != std::string::npos && !Trim(cleaned.substr(newline)).empty())
        {
            cleaned = Trim(cleaned.substr(newline));
        }
        break;
    }

    // A single fully-quoted line is the line, not a quotation of it.
    if (cleaned.size() > 1 && cleaned.front() == '"' && cleaned.back() == '"' &&
        cleaned.find('"', 1) == cleaned.size() - 1)
    {
        cleaned = Trim(cleaned.substr(1, cleaned.size() - 2));
    }
    return cleaned;
}

} // namespace revia::content
