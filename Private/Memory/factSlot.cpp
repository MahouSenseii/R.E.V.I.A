#include "Memory/factSlot.h"
#include "Core/speechAttribution.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace revia::memory
{
namespace
{
std::string Lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string Trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n.!?,;:");
    return std::string(text.substr(first, last - first + 1));
}

// The person's own words: quoted speech removed, lower case, curly apostrophes made plain.
std::string OwnWords(const std::string& input)
{
    std::string text = Lower(conversation::ReadSpeechAttribution(input).userAuthoredText);
    for (std::size_t at = text.find("\xE2\x80\x99"); at != std::string::npos; at = text.find("\xE2\x80\x99", at))
        text.replace(at, 3, "'");
    return " " + text + " ";
}

bool Word(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '\''; }

// Whole-word (or whole-phrase) occurrence.
bool Contains(const std::string& text, std::string_view phrase)
{
    if (phrase.empty()) return false;
    for (std::size_t at = text.find(phrase); at != std::string::npos; at = text.find(phrase, at + 1))
    {
        const bool before = at == 0 || !Word(text[at - 1]);
        const std::size_t end = at + phrase.size();
        const bool after = end >= text.size() || !Word(text[end]);
        if (before && after) return true;
    }
    return false;
}

bool ContainsAny(const std::string& text, std::initializer_list<std::string_view> phrases)
{
    return std::any_of(phrases.begin(), phrases.end(), [&](std::string_view p) { return Contains(text, p); });
}
} // namespace

const std::vector<std::string>& FactAttributes()
{
    static const std::vector<std::string> attributes = {
        "home city", "job", "employer", "school", "car colour", "car model", "phone model", "pet name", "pet age", "age",
        "birthday", "partner name", "favourite colour", "favourite food", "favourite drink"};
    return attributes;
}

std::optional<std::string> CanonicalFactAttribute(std::string_view attribute)
{
    std::string value = Lower(Trim(attribute));
    for (std::size_t at = value.find("color"); at != std::string::npos; at = value.find("color", at + 1))
        value.replace(at, 5, "colour");
    for (std::size_t at = value.find("favorite"); at != std::string::npos; at = value.find("favorite", at + 1))
        value.replace(at, 8, "favourite");
    if (value == "city" || value == "hometown") value = "home city";
    const auto& list = FactAttributes();
    if (std::find(list.begin(), list.end(), value) == list.end()) return std::nullopt;
    return value;
}

std::string NormalizeFactObject(std::string_view object)
{
    std::string value = Lower(Trim(object));
    for (const std::string_view article : {"my ", "the ", "a ", "an "})
        if (value.starts_with(article)) value.erase(0, article.size());
    if (value.size() > 40) value.resize(40);
    return value;
}

std::string_view FactChangeName(const FactChange change)
{
    switch (change)
    {
        case FactChange::Correction: return "correction";
        case FactChange::Change: return "change";
        case FactChange::Future: return "future";
        default: return "statement";
    }
}

std::optional<FactChange> ParseFactChange(const std::string_view name)
{
    if (name == "statement") return FactChange::Statement;
    if (name == "correction") return FactChange::Correction;
    if (name == "change") return FactChange::Change;
    if (name == "future") return FactChange::Future;
    return std::nullopt;
}

bool FactValueInOwnWords(const std::string_view value, const std::string& userInput)
{
    const std::string wanted = Lower(Trim(value));
    if (wanted.empty() || wanted.size() > 60) return false;
    return Contains(OwnWords(userInput), wanted);
}

std::string CorrectedAwayValue(const std::string& userInput)
{
    const std::string text = OwnWords(userInput);
    for (const std::string_view negation : {" not ", " isn't ", " wasn't ", " instead of "})
    {
        const auto at = text.find(negation);
        if (at == std::string::npos) continue;
        // The next word, up to punctuation: "not blue." -> "blue". One word keeps this conservative.
        std::size_t start = at + negation.size();
        while (start < text.size() && text[start] == ' ') ++start;
        std::size_t end = start;
        while (end < text.size() && Word(text[end])) ++end;
        std::string value = text.substr(start, end - start);
        if (value.size() >= 2 && value != "a" && value != "the" && value != "my" && value != "sure" && value != "really")
            return value;
    }
    return {};
}

FactChange ClassifyFactChange(const std::string& userInput, const std::string_view newValue, const std::string_view priorValue)
{
    const std::string text = OwnWords(userInput);
    const std::string next = Lower(Trim(newValue));
    const std::string prior = Lower(Trim(priorValue));
    // A plan is never current, even if it also mentions the old value.
    if (ContainsAny(text, {"will", "i'll", "going to", "gonna", "next week", "next month", "next year", "tomorrow", "soon",
            "planning to", "plan to", "about to", "starting next", "from next"}))
        return FactChange::Future;
    if (prior.empty() || prior == next) return FactChange::Statement;
    // The old value negated in the person's own words: "not blue", "isn't blue", "wasn't blue".
    for (const std::string_view negation : {"not ", "isn't ", "wasn't ", "never "})
        if (Contains(text, std::string(negation) + prior)) return FactChange::Correction;
    if (ContainsAny(text, {"actually", "i meant", "correction", "i misspoke", "my mistake", "i was wrong", "i got that wrong",
            "sorry, i", "wait, i", "scratch that"}))
        return FactChange::Correction;
    if (ContainsAny(text, {"now", "no longer", "moved", "i've moved", "switched", "changed", "these days", "anymore", "just got",
            "new job", "new car", "started at"}))
        return FactChange::Change;
    return FactChange::Statement;
}

// Rules, applied to versions in storage order:
// 1. A future version newer than every current-tense version is a plan, never the current value.
// 2. Owner-retired (reviewed historical) versions never compete.
// 3. The newest current-tense version L decides:
//    - L is a clear correction or change and an earlier, different value exists: L is current,
//      flagged unreviewed (or reviewed, if the owner already approved L).
//    - L restates the same value as everything else: stated (or reviewed, if approved).
//    - L is a plain statement that disagrees with an earlier value: unresolved, no current value,
//      unless the owner approved one of them, which then stands.
FactView ResolveFactSlot(std::vector<FactVersion> versions, const std::vector<std::pair<std::string, std::string>>& rejectedPairs)
{
    std::stable_sort(versions.begin(), versions.end(), [](const auto& a, const auto& b) { return a.sequence < b.sequence; });
    FactView view;
    view.versions = versions;
    // An owner-retired value stays retired only behind the approved current value. If the person
    // later clearly returns to it ("blue after all"), that newer version competes again.
    std::int64_t approvedAt = -1;
    for (const auto& v : versions)
        if (v.reviewedCurrent) approvedAt = std::max(approvedAt, v.sequence);
    std::vector<FactVersion> present;
    for (const auto& v : versions)
        if (v.change != FactChange::Future && !(v.reviewedHistorical && v.sequence < approvedAt)) present.push_back(v);
    const std::int64_t newestPresent = present.empty() ? -1 : present.back().sequence;
    for (const auto& v : versions)
        if (v.change == FactChange::Future && v.sequence > newestPresent) view.planned = v;
    if (present.empty()) return view;

    const auto& last = present.back();
    std::vector<FactVersion> differing;
    for (auto it = present.rbegin(); it != present.rend(); ++it)
        if (Lower(it->value) != Lower(last.value)) differing.push_back(*it);

    if (differing.empty())
    {
        view.current = last;
        const bool approved = std::any_of(present.begin(), present.end(), [](const auto& v) { return v.reviewedCurrent; });
        view.state = approved ? FactState::Reviewed : FactState::Stated;
        return view;
    }
    const bool rejected = std::any_of(rejectedPairs.begin(), rejectedPairs.end(), [&](const auto& pair)
        { return pair.second == last.memoryId && pair.first == differing.front().memoryId; });
    if ((last.change == FactChange::Correction || last.change == FactChange::Change) && !rejected)
    {
        view.current = last;
        view.replaced = differing;
        view.state = last.reviewedCurrent ? FactState::Reviewed
                     : last.change == FactChange::Correction ? FactState::UnreviewedCorrection : FactState::UnreviewedChange;
        return view;
    }
    // A plain disagreement. An owner-approved value still stands; otherwise nothing is current.
    for (auto it = present.rbegin(); it != present.rend(); ++it)
        if (it->reviewedCurrent)
        {
            view.current = *it;
            view.state = FactState::Reviewed;
            for (const auto& v : present)
                if (Lower(v.value) != Lower(it->value)) view.conflicting.insert(view.conflicting.begin(), v);
            return view;
        }
    view.state = FactState::Unresolved;
    for (auto it = present.rbegin(); it != present.rend(); ++it) view.conflicting.push_back(*it);
    return view;
}

bool FactSummaryFitsAttribute(const std::string& summary, const std::string& attribute)
{
    static const std::vector<std::pair<std::string, std::vector<std::string_view>>> cues = {
        {"home city", {"live", "lives", "living", "city", "from", "moved", "home", "hometown"}},
        {"job", {"work", "works", "working", "job", "employed", "profession", "career", "as a", "as an"}},
        {"employer", {"work", "works", "working", "employer", "company", "employed", "job"}},
        {"school", {"school", "study", "studies", "studying", "student", "university", "college"}},
        {"car colour", {"car", "vehicle", "truck", "van", "drive", "drives", "colour", "color"}},
        {"car model", {"car", "vehicle", "truck", "van", "drive", "drives"}},
        {"phone model", {"phone"}},
        {"pet name", {"dog", "cat", "pet", "puppy", "kitten", "named", "called", "parrot", "rabbit", "hamster"}},
        {"pet age", {"dog", "cat", "pet", "puppy", "kitten", "parrot", "rabbit", "hamster"}},
        {"age", {"years old", "age", "aged"}},
        {"birthday", {"birthday", "born"}},
        {"partner name", {"partner", "wife", "husband", "girlfriend", "boyfriend", "spouse", "fiance", "fiancee"}},
        {"favourite colour", {"favourite colour", "favorite color", "favourite color", "favorite colour"}},
        {"favourite food", {"favourite food", "favorite food", "favourite dish", "favorite dish"}},
        {"favourite drink", {"favourite drink", "favorite drink"}}};
    const std::string text = " " + Lower(summary) + " ";
    for (const auto& [name, words] : cues)
        if (name == attribute)
            return std::any_of(words.begin(), words.end(), [&](std::string_view w) { return Contains(text, w); });
    return false;
}

std::string DescribeFactForPrompt(const FactView& view)
{
    std::string label = view.attribute + (view.object.empty() ? "" : " of " + view.object);
    std::string line = "- [fact: " + label + "] ";
    const auto earlier = [&](const std::vector<FactVersion>& list)
    {
        std::string out;
        for (std::size_t i = 0; i < list.size() && i < 3; ++i) out += (i ? ", " : "") + list[i].value;
        return out;
    };
    switch (view.state)
    {
        case FactState::Reviewed:
            line += "Current: " + view.current->summary + " (approved by the owner";
            if (!view.conflicting.empty()) line += "; later statements disagree: " + earlier(view.conflicting);
            line += ")";
            break;
        case FactState::UnreviewedCorrection:
            line += "Current: " + view.current->summary + " (the user corrected this from " + earlier(view.replaced) +
                    "; the correction is not yet reviewed by the owner, so the earlier value is kept on record)";
            break;
        case FactState::UnreviewedChange:
            line += "Current: " + view.current->summary + " (the user said this changed from " + earlier(view.replaced) +
                    "; not yet reviewed by the owner)";
            break;
        case FactState::Unresolved:
            line += "Unresolved: the user has said " + earlier(view.conflicting) +
                    " (newest first) without saying which is right; do not pick one, ask if it matters";
            break;
        default:
            if (view.current) line += "Current: " + view.current->summary;
            break;
    }
    if (view.planned) line += (view.current || view.state == FactState::Unresolved ? " " : "") +
                              std::string("Planned, not current yet: ") + view.planned->summary;
    return line;
}
} // namespace revia::memory
