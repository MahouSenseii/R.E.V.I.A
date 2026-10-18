#include "Evaluation/goldenSet.h"

#include "Agents/conversationQualityMonitor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>

namespace revia::evaluation
{
namespace
{
using Json = nlohmann::json;

constexpr std::size_t MaximumSetBytes = 4 * 1024 * 1024;
constexpr std::size_t MaximumCases = 2000;
constexpr std::size_t MaximumTurns = 12;

struct KindSpec
{
    GoldenCheckKind kind;
    const char* name;
    std::set<std::string> fields;
};

const std::vector<KindSpec>& KindSpecs()
{
    static const std::vector<KindSpec> specs = {
        {GoldenCheckKind::Exact, "exact", {"kind", "value", "caseSensitive"}},
        {GoldenCheckKind::Regex, "regex", {"kind", "pattern", "caseSensitive"}},
        {GoldenCheckKind::NotRegex, "not_regex", {"kind", "pattern", "caseSensitive"}},
        {GoldenCheckKind::Contains, "contains", {"kind", "values", "caseSensitive"}},
        {GoldenCheckKind::ContainsAny, "contains_any", {"kind", "values", "caseSensitive"}},
        {GoldenCheckKind::NotContains, "not_contains", {"kind", "values", "caseSensitive"}},
        {GoldenCheckKind::MaxSentences, "max_sentences", {"kind", "limit"}},
        {GoldenCheckKind::ExactSentences, "exact_sentences", {"kind", "count"}},
        {GoldenCheckKind::MaxWords, "max_words", {"kind", "limit"}},
        {GoldenCheckKind::ListItems, "list_items", {"kind", "count", "min", "max"}},
        {GoldenCheckKind::JsonValid, "json_valid", {"kind", "requiredKeys"}},
        {GoldenCheckKind::Number, "number", {"kind", "value", "tolerance", "match", "aliases"}},
        {GoldenCheckKind::Contract, "contract", {"kind", "name"}}};
    return specs;
}

// Duplicate keys are refused rather than resolved, so an edited line cannot silently
// keep an old expectation beside a new one.
Json ParseStrict(const std::string& bytes)
{
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes,
        [&](int depth, Json::parse_event_t event, Json& parsed)
        {
            if (depth > 32)
                throw std::runtime_error("JSON exceeds its depth limit.");
            if (event == Json::parse_event_t::object_start)
                keys.emplace_back();
            else if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
                throw std::runtime_error("Duplicate JSON key: " + parsed.get<std::string>());
            else if (event == Json::parse_event_t::object_end)
                keys.pop_back();
            return true;
        });
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Curly apostrophes and quotes are how models usually write "don't"; an authored value
// with a straight apostrophe must still match it.
std::string NormalizeQuotes(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (index + 2 < text.size() && static_cast<unsigned char>(text[index]) == 0xE2 && static_cast<unsigned char>(text[index + 1]) == 0x80)
        {
            const auto third = static_cast<unsigned char>(text[index + 2]);
            if (third == 0x98 || third == 0x99)
            {
                result.push_back('\'');
                index += 2;
                continue;
            }
            if (third == 0x9C || third == 0x9D)
            {
                result.push_back('"');
                index += 2;
                continue;
            }
        }
        result.push_back(text[index]);
    }
    return result;
}

std::string Comparable(const std::string& text, const bool caseSensitive)
{
    const auto normalized = NormalizeQuotes(text);
    return caseSensitive ? normalized : Lower(normalized);
}

std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool IsWordCharacter(const char value)
{
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
}

bool ContainsWord(const std::string& haystack, const std::string& word)
{
    for (std::size_t found = haystack.find(word); !word.empty() && found != std::string::npos; found = haystack.find(word, found + 1))
    {
        const bool startOk = found == 0 || !IsWordCharacter(haystack[found - 1]);
        const std::size_t end = found + word.size();
        if (startOk && (end >= haystack.size() || !IsWordCharacter(haystack[end])))
            return true;
    }
    return false;
}

std::size_t ReadCount(const Json& value, const std::string& field)
{
    if (!value.is_number_unsigned() || value.get<std::size_t>() == 0 || value.get<std::size_t>() > 10000)
        throw std::runtime_error("\"" + field + "\" must be a positive integer.");
    return value.get<std::size_t>();
}

std::vector<std::string> ReadStrings(const Json& value, const std::string& field, const bool allowEmpty)
{
    if (!value.is_array() || (!allowEmpty && value.empty()))
        throw std::runtime_error("\"" + field + "\" must be a nonempty array of strings.");
    std::vector<std::string> result;
    for (const auto& item : value)
    {
        if (!item.is_string() || Trim(item.get<std::string>()).empty())
            throw std::runtime_error("\"" + field + "\" must contain nonempty strings.");
        result.push_back(item.get<std::string>());
    }
    return result;
}

std::regex::flag_type RegexFlags(const bool caseSensitive)
{
    return caseSensitive ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase;
}

GoldenCheck ParseCheck(const Json& entry)
{
    if (!entry.is_object() || !entry.contains("kind") || !entry.at("kind").is_string())
        throw std::runtime_error("A check needs a string \"kind\".");
    const auto name = entry.at("kind").get<std::string>();
    const auto spec = std::find_if(KindSpecs().begin(), KindSpecs().end(), [&](const KindSpec& item) { return name == item.name; });
    if (spec == KindSpecs().end())
        throw std::runtime_error("Unknown check kind: " + name);
    for (const auto& [key, unused] : entry.items())
        if (!spec->fields.contains(key))
            throw std::runtime_error("Check kind " + name + " does not take the field \"" + key + "\".");

    GoldenCheck check;
    check.kind = spec->kind;
    if (entry.contains("caseSensitive"))
    {
        if (!entry.at("caseSensitive").is_boolean())
            throw std::runtime_error("\"caseSensitive\" must be true or false.");
        check.caseSensitive = entry.at("caseSensitive").get<bool>();
    }
    const auto require = [&](const char* field) -> const Json&
    {
        if (!entry.contains(field))
            throw std::runtime_error("Check kind " + name + " requires \"" + field + "\".");
        return entry.at(field);
    };
    switch (check.kind)
    {
    case GoldenCheckKind::Exact:
        if (!require("value").is_string() || Trim(entry.at("value").get<std::string>()).empty())
            throw std::runtime_error("\"value\" must be nonempty text.");
        check.values = {entry.at("value").get<std::string>()};
        break;
    case GoldenCheckKind::Regex:
    case GoldenCheckKind::NotRegex:
        if (!require("pattern").is_string() || entry.at("pattern").get<std::string>().empty())
            throw std::runtime_error("\"pattern\" must be nonempty text.");
        check.pattern = entry.at("pattern").get<std::string>();
        try
        {
            (void)std::regex(check.pattern, RegexFlags(check.caseSensitive));
        }
        catch (const std::regex_error& error)
        {
            throw std::runtime_error("Invalid regular expression " + check.pattern + ": " + error.what());
        }
        break;
    case GoldenCheckKind::Contains:
    case GoldenCheckKind::ContainsAny:
    case GoldenCheckKind::NotContains:
        check.values = ReadStrings(require("values"), "values", false);
        break;
    case GoldenCheckKind::MaxSentences:
    case GoldenCheckKind::MaxWords:
        check.maximum = ReadCount(require("limit"), "limit");
        break;
    case GoldenCheckKind::ExactSentences:
        check.minimum = check.maximum = ReadCount(require("count"), "count");
        break;
    case GoldenCheckKind::ListItems:
        if (entry.contains("count"))
        {
            if (entry.contains("min") || entry.contains("max"))
                throw std::runtime_error("list_items takes either \"count\" or \"min\"/\"max\".");
            check.minimum = check.maximum = ReadCount(entry.at("count"), "count");
        }
        else
        {
            if (!entry.contains("min") && !entry.contains("max"))
                throw std::runtime_error("list_items requires \"count\", \"min\" or \"max\".");
            check.minimum = entry.contains("min") ? ReadCount(entry.at("min"), "min") : 0;
            check.maximum = entry.contains("max") ? ReadCount(entry.at("max"), "max") : 0;
            if (check.maximum > 0 && check.minimum > check.maximum)
                throw std::runtime_error("list_items \"min\" exceeds \"max\".");
        }
        break;
    case GoldenCheckKind::JsonValid:
        if (entry.contains("requiredKeys"))
            check.requiredKeys = ReadStrings(entry.at("requiredKeys"), "requiredKeys", false);
        break;
    case GoldenCheckKind::Number:
        if (!require("value").is_number() || !std::isfinite(entry.at("value").get<double>()))
            throw std::runtime_error("\"value\" must be a finite number.");
        check.number = entry.at("value").get<double>();
        if (entry.contains("tolerance"))
        {
            if (!entry.at("tolerance").is_number() || entry.at("tolerance").get<double>() < 0.0)
                throw std::runtime_error("\"tolerance\" must be a nonnegative number.");
            check.tolerance = entry.at("tolerance").get<double>();
        }
        if (entry.contains("match"))
        {
            const auto& match = entry.at("match");
            if (!match.is_string() || (match != "any" && match != "last"))
                throw std::runtime_error("\"match\" must be \"any\" or \"last\".");
            check.lastNumberOnly = match == "last";
        }
        if (entry.contains("aliases"))
            check.values = ReadStrings(entry.at("aliases"), "aliases", false);
        break;
    case GoldenCheckKind::Contract:
    {
        if (!require("name").is_string() || !ParseCheckKind(entry.at("name").get<std::string>(), check.contract))
            throw std::runtime_error("Unknown contract check: " + entry.at("name").dump());
        // These take values or limits, which the golden kinds above already express.
        if (check.contract == CheckKind::MaxSentences || check.contract == CheckKind::MustContainAny ||
            check.contract == CheckKind::MustNotContain)
            throw std::runtime_error("Use the golden check kind instead of contract " + entry.at("name").get<std::string>() + ".");
        break;
    }
    }
    return check;
}

GoldenCase ParseCase(const Json& entry)
{
    static const std::set<std::string> fields = {"id", "category", "turns", "rubric"};
    if (!entry.is_object())
        throw std::runtime_error("A case must be a JSON object.");
    for (const auto& [key, unused] : entry.items())
        if (!fields.contains(key))
            throw std::runtime_error("A case does not take the field \"" + key + "\".");
    GoldenCase item;
    if (!entry.contains("id") || !entry.at("id").is_string() || entry.at("id").get<std::string>().empty())
        throw std::runtime_error("A case needs a nonempty \"id\".");
    item.id = entry.at("id").get<std::string>();
    const auto fail = [&](const std::string& message) { throw std::runtime_error("Case " + item.id + ": " + message); };
    if (!entry.contains("category") || !entry.at("category").is_string())
        fail("missing \"category\".");
    item.category = entry.at("category").get<std::string>();
    const auto& categories = GoldenCategories();
    if (std::find(categories.begin(), categories.end(), item.category) == categories.end())
        fail("unknown category " + item.category + ".");
    if (!entry.contains("rubric") || !entry.at("rubric").is_string() || Trim(entry.at("rubric").get<std::string>()).empty())
        fail("missing \"rubric\".");
    item.rubric = entry.at("rubric").get<std::string>();
    if (!entry.contains("turns") || !entry.at("turns").is_array() || entry.at("turns").empty() ||
        entry.at("turns").size() > MaximumTurns)
        fail("\"turns\" must hold 1 to " + std::to_string(MaximumTurns) + " turns.");
    bool anyCheck = false;
    for (const auto& turnEntry : entry.at("turns"))
    {
        if (!turnEntry.is_object())
            fail("a turn must be an object.");
        for (const auto& [key, unused] : turnEntry.items())
            if (key != "input" && key != "checks")
                fail("a turn does not take the field \"" + key + "\".");
        if (!turnEntry.contains("input") || !turnEntry.at("input").is_string() || Trim(turnEntry.at("input").get<std::string>()).empty())
            fail("a turn has no input.");
        GoldenTurn turn;
        turn.input = turnEntry.at("input").get<std::string>();
        if (turnEntry.contains("checks"))
        {
            if (!turnEntry.at("checks").is_array())
                fail("\"checks\" must be an array.");
            for (const auto& checkEntry : turnEntry.at("checks"))
            {
                try
                {
                    turn.checks.push_back(ParseCheck(checkEntry));
                }
                catch (const std::exception& error)
                {
                    fail(error.what());
                }
            }
        }
        anyCheck = anyCheck || !turn.checks.empty();
        item.turns.push_back(std::move(turn));
    }
    // The final reply is what the judge reads; it must also carry a deterministic floor.
    if (!anyCheck || item.turns.back().checks.empty())
        fail("the final turn needs at least one check.");
    return item;
}
}

std::string ToString(const GoldenCheckKind kind)
{
    for (const auto& spec : KindSpecs())
        if (spec.kind == kind)
            return spec.name;
    return "unknown";
}

const std::vector<std::string>& GoldenCategories()
{
    static const std::vector<std::string> categories = {"code", "math", "factual", "instruction", "continuity", "safety", "persona"};
    return categories;
}

bool ParseGoldenSet(const std::string& jsonLines, std::vector<GoldenCase>& outCases, std::string& outError)
{
    try
    {
        if (jsonLines.size() > MaximumSetBytes)
            throw std::runtime_error("The golden set exceeds its byte limit.");
        std::vector<GoldenCase> loaded;
        std::set<std::string> ids;
        std::istringstream stream(jsonLines);
        std::string line;
        std::size_t lineNumber = 0;
        while (std::getline(stream, line))
        {
            ++lineNumber;
            if (Trim(line).empty())
                continue;
            try
            {
                auto item = ParseCase(ParseStrict(line));
                if (!ids.insert(item.id).second)
                    throw std::runtime_error("duplicate case id " + item.id + ".");
                loaded.push_back(std::move(item));
            }
            catch (const std::exception& error)
            {
                throw std::runtime_error("Line " + std::to_string(lineNumber) + ": " + error.what());
            }
        }
        if (loaded.empty() || loaded.size() > MaximumCases)
            throw std::runtime_error("The golden set must hold 1 to " + std::to_string(MaximumCases) + " cases.");
        outCases = std::move(loaded);
        outError.clear();
        return true;
    }
    catch (const std::exception& error)
    {
        outError = error.what();
        return false;
    }
}

bool LoadGoldenSet(const std::filesystem::path& path, std::vector<GoldenCase>& outCases, std::string& outError)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        outError = "No golden set at " + path.string() + '.';
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return ParseGoldenSet(bytes, outCases, outError);
}

std::vector<double> ExtractNumbers(const std::string& text)
{
    std::vector<double> numbers;
    const auto digit = [&](const std::size_t at) { return at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0; };
    for (std::size_t index = 0; index < text.size();)
    {
        if (!digit(index))
        {
            ++index;
            continue;
        }
        // A hyphen is a sign only where it cannot be joining two words or a range.
        const bool negative = index > 0 && text[index - 1] == '-' && (index < 2 || !std::isalnum(static_cast<unsigned char>(text[index - 2])));
        std::string literal;
        std::size_t end = index;
        while (digit(end))
            literal.push_back(text[end++]);
        while (end < text.size() && text[end] == ',' && digit(end + 1) && digit(end + 2) && digit(end + 3) && !digit(end + 4))
        {
            literal.append(text, end + 1, 3);
            end += 4;
        }
        if (end < text.size() && text[end] == '.' && digit(end + 1))
        {
            literal.push_back('.');
            ++end;
            while (digit(end))
                literal.push_back(text[end++]);
        }
        const double value = std::strtod(literal.c_str(), nullptr);
        numbers.push_back(negative ? -value : value);
        index = end;
    }
    return numbers;
}

std::size_t CountListItems(const std::string& text)
{
    std::size_t items = 0;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        const auto trimmed = Trim(line);
        if (trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '*') && trimmed[1] == ' ')
            ++items;
        else if (trimmed.rfind("\xE2\x80\xA2", 0) == 0)
            ++items;
        else
        {
            std::size_t digits = 0;
            while (digits < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[digits])) != 0)
                ++digits;
            if (digits > 0 && digits + 1 < trimmed.size() && (trimmed[digits] == '.' || trimmed[digits] == ')') &&
                trimmed[digits + 1] == ' ')
                ++items;
        }
    }
    return items;
}

std::size_t CountWords(const std::string& text)
{
    std::size_t words = 0;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token)
        if (std::any_of(token.begin(), token.end(), [](const unsigned char c) { return std::isalnum(c) != 0 || c >= 0x80; }))
            ++words;
    return words;
}

std::vector<std::string> ApplyGoldenCheck(
    const GoldenCheck& check, const std::string& input, const std::string& reply, const std::vector<std::string>& earlierOpenings)
{
    std::vector<std::string> failures;
    const auto subject = Comparable(reply, check.caseSensitive);
    switch (check.kind)
    {
    case GoldenCheckKind::Exact:
    {
        auto trimmed = Trim(subject);
        if (!trimmed.empty() && trimmed.back() == '.')
            trimmed.pop_back();
        auto expected = Trim(Comparable(check.values.front(), check.caseSensitive));
        if (!expected.empty() && expected.back() == '.')
            expected.pop_back();
        if (trimmed != expected)
            failures.push_back("expected exactly \"" + check.values.front() + "\"");
        break;
    }
    case GoldenCheckKind::Regex:
    case GoldenCheckKind::NotRegex:
    {
        try
        {
            const bool found = std::regex_search(NormalizeQuotes(reply), std::regex(check.pattern, RegexFlags(check.caseSensitive)));
            if (found != (check.kind == GoldenCheckKind::Regex))
                failures.push_back((found ? "matched the forbidden pattern " : "did not match the pattern ") + check.pattern);
        }
        catch (const std::regex_error&)
        {
            failures.push_back("the pattern " + check.pattern + " could not be evaluated on this reply");
        }
        break;
    }
    case GoldenCheckKind::Contains:
        for (const auto& value : check.values)
            if (subject.find(Comparable(value, check.caseSensitive)) == std::string::npos)
                failures.push_back("did not say \"" + value + "\"");
        break;
    case GoldenCheckKind::ContainsAny:
    {
        const bool found = std::any_of(check.values.begin(), check.values.end(),
            [&](const std::string& value) { return subject.find(Comparable(value, check.caseSensitive)) != std::string::npos; });
        if (!found)
        {
            std::string message = "said none of";
            for (const auto& value : check.values)
                message += " \"" + value + "\"";
            failures.push_back(message);
        }
        break;
    }
    case GoldenCheckKind::NotContains:
        for (const auto& value : check.values)
            if (subject.find(Comparable(value, check.caseSensitive)) != std::string::npos)
                failures.push_back("said \"" + value + "\"");
        break;
    case GoldenCheckKind::MaxSentences:
    case GoldenCheckKind::ExactSentences:
    {
        const auto count = ConversationEvaluator::CountSentences(reply);
        if (count > check.maximum || count < check.minimum)
            failures.push_back("answered in " + std::to_string(count) + " sentences where the case expects " +
                               (check.minimum == check.maximum ? "exactly " : "at most ") + std::to_string(check.maximum));
        break;
    }
    case GoldenCheckKind::MaxWords:
    {
        const auto count = CountWords(reply);
        if (count > check.maximum)
            failures.push_back("used " + std::to_string(count) + " words where the case allows " + std::to_string(check.maximum));
        break;
    }
    case GoldenCheckKind::ListItems:
    {
        const auto count = CountListItems(reply);
        if (count < check.minimum || (check.maximum > 0 && count > check.maximum))
            failures.push_back("gave " + std::to_string(count) + " list items where the case expects " +
                               (check.minimum == check.maximum ? std::to_string(check.maximum)
                                                               : std::to_string(check.minimum) + " to " +
                                                                     (check.maximum > 0 ? std::to_string(check.maximum) : "any")));
        break;
    }
    case GoldenCheckKind::JsonValid:
    {
        try
        {
            const auto parsed = ParseStrict(Trim(reply));
            for (const auto& key : check.requiredKeys)
                if (!parsed.is_object() || !parsed.contains(key))
                    failures.push_back("the JSON has no \"" + key + "\" field");
        }
        catch (const std::exception&)
        {
            failures.emplace_back("the reply is not one valid JSON value");
        }
        break;
    }
    case GoldenCheckKind::Number:
    {
        auto numbers = ExtractNumbers(reply);
        if (check.lastNumberOnly && numbers.size() > 1)
            numbers.erase(numbers.begin(), numbers.end() - 1);
        const double allowed = std::max(check.tolerance, 1e-9 * std::max(1.0, std::fabs(check.number)));
        const bool numeric = std::any_of(numbers.begin(), numbers.end(), [&](const double value) { return std::fabs(value - check.number) <= allowed; });
        const auto lowered = Lower(NormalizeQuotes(reply));
        const bool spelled = !check.lastNumberOnly && std::any_of(check.values.begin(), check.values.end(),
                                                         [&](const std::string& alias) { return ContainsWord(lowered, Lower(alias)); });
        if (!numeric && !spelled)
        {
            std::ostringstream expected;
            expected << check.number;
            if (check.tolerance > 0.0)
                expected << " (within " << check.tolerance << ")";
            failures.push_back((check.lastNumberOnly ? "the final number is not " : "never gave the number ") + expected.str());
        }
        break;
    }
    case GoldenCheckKind::Contract:
    {
        EvaluationCheck contract;
        contract.kind = check.contract;
        failures = ConversationEvaluator::Apply(contract, input, reply, earlierOpenings);
        break;
    }
    }
    return failures;
}

std::vector<std::string> CheckGoldenTurn(const GoldenTurn& turn, const std::string& reply, const std::vector<std::string>& earlierOpenings)
{
    std::vector<std::string> failures;
    for (const auto& check : turn.checks)
        for (auto& failure : ApplyGoldenCheck(check, turn.input, reply, earlierOpenings))
            failures.push_back(ToString(check.kind) + ": " + std::move(failure));
    return failures;
}

EvaluationCase ToEvaluationCase(const GoldenCase& item)
{
    EvaluationCase result;
    result.id = item.id;
    result.title = item.category;
    result.clause = "golden set: " + item.category;
    for (const auto& turn : item.turns)
        result.turns.push_back({turn.input, {EvaluationCheck{}}});
    return result;
}
}
