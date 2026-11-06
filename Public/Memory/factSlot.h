#pragma once

// Fact slots: one current value per (person, attribute, object), so a corrected fact can say
// which value it replaces instead of sitting beside the old one. Versioning only: nothing here
// approves a revision. The owner's review still decides when an old row becomes historical; until
// then, a clear correction or change made by the person themselves is shown as the newest value,
// flagged unreviewed, and anything less clear is shown as unresolved.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace revia::memory
{
// A single-value attribute of the speaker, or of one specific thing of theirs ("Civic", "dog").
struct FactSlot
{
    std::string attribute;
    std::string object;
    std::string value;
};

// How the person's own words relate the new value to the slot's earlier value. Decided by the
// host from the raw message, never taken from the model.
enum class FactChange
{
    Statement,   // a plain statement; if it disagrees with an earlier value, that stays unresolved
    Correction,  // "not blue", "actually green", "I meant green": the earlier value was wrong
    Change,      // "now green", "I moved to Porto": the earlier value was true until now
    Future       // "next month", "going to": not current yet
};

// The closed list of single-value attributes a slot may use; anything else gets no slot, which
// fails safe (no supersession) rather than merging two different facts.
[[nodiscard]] std::optional<std::string> CanonicalFactAttribute(std::string_view attribute);
[[nodiscard]] const std::vector<std::string>& FactAttributes();
[[nodiscard]] std::string NormalizeFactObject(std::string_view object);
[[nodiscard]] std::string_view FactChangeName(FactChange change);
[[nodiscard]] std::optional<FactChange> ParseFactChange(std::string_view name);

// True when the value occurs in the person's own words (quoted speech excluded), so a slot the
// model invented, or one taken from somebody else's quoted words, is dropped.
[[nodiscard]] bool FactValueInOwnWords(std::string_view value, const std::string& userInput);

// Host-side reading of the person's own words. `priorValue` is the slot's current or latest value
// before this message, empty when the slot is new.
[[nodiscard]] FactChange ClassifyFactChange(const std::string& userInput, std::string_view newValue, std::string_view priorValue);

struct FactVersion
{
    std::string memoryId;
    std::string summary;
    std::string value;
    FactChange change = FactChange::Statement;
    std::int64_t sequence = 0;     // storage order; larger is newer
    bool reviewedCurrent = false;  // the owner approved this row as the current revision
    bool reviewedHistorical = false;
    std::string evidence;
};

enum class FactState
{
    Stated,                // one value (possibly repeated); no conflict
    Reviewed,              // the owner approved the current value
    UnreviewedCorrection,  // the person clearly corrected it; review still pending
    UnreviewedChange,      // the person clearly said it changed; review still pending
    Unresolved             // differing values without a clear correction or change
};

struct FactView
{
    std::string subjectKey;
    std::string attribute;
    std::string object;
    FactState state = FactState::Stated;
    std::optional<FactVersion> current;   // empty when unresolved or only a plan exists
    std::vector<FactVersion> replaced;    // earlier differing values, newest first (corrections/changes)
    std::vector<FactVersion> conflicting; // unresolved values, newest first
    std::optional<FactVersion> planned;   // a future change newer than every current-tense version
    std::vector<FactVersion> versions;    // every version, oldest first
};

// Pure resolution of one slot's versions (oldest first). See factSlot.cpp for the rules.
[[nodiscard]] FactView ResolveFactSlot(std::vector<FactVersion> versions);

// One prompt line describing the slot in plain words for the model.
[[nodiscard]] std::string DescribeFactForPrompt(const FactView& view);
} // namespace revia::memory
