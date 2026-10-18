#pragma once

#include "Evaluation/conversationEvaluation.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace revia::evaluation
{
// The answer-quality golden set: authored cases with exact expectations where an
// expectation can be stated exactly, plus a short rubric for the model judge.
//
// The file is JSON Lines, one case per line, so a reviewer can see exactly which case a
// change touched. docs/EVALUATION_GATE.md documents the schema.
enum class GoldenCheckKind
{
    // The trimmed reply equals the value, ignoring one trailing full stop.
    Exact,
    Regex,
    NotRegex,
    // Every value appears.
    Contains,
    // At least one value appears.
    ContainsAny,
    // No value appears.
    NotContains,
    MaxSentences,
    ExactSentences,
    MaxWords,
    // Lines that start with a bullet or a list number.
    ListItems,
    // The whole reply is one JSON value, optionally an object with required keys.
    JsonValid,
    // A number in the reply lies within the tolerance of the expected value.
    Number,
    // One of the existing conversation contract checks, applied exactly as /eval does.
    Contract
};

[[nodiscard]] std::string ToString(GoldenCheckKind kind);

struct GoldenCheck
{
    GoldenCheckKind kind = GoldenCheckKind::Contains;
    std::vector<std::string> values;
    std::string pattern;
    bool caseSensitive = false;
    // Count bounds for the counting kinds; ExactSentences and an exact ListItems count set both.
    std::size_t minimum = 0;
    std::size_t maximum = 0;
    double number = 0.0;
    double tolerance = 0.0;
    // Number: judge only the last number in the reply, not any number.
    bool lastNumberOnly = false;
    std::vector<std::string> requiredKeys;
    CheckKind contract = CheckKind::NotEmpty;
};

struct GoldenTurn
{
    std::string input;
    std::vector<GoldenCheck> checks;
};

struct GoldenCase
{
    std::string id;
    std::string category;
    std::vector<GoldenTurn> turns;
    // What the judge should look for. Required: a case with only phrase checks is the
    // weakness the gate exists to fix.
    std::string rubric;
};

[[nodiscard]] const std::vector<std::string>& GoldenCategories();

// Strict: an unknown kind, field or category, a duplicate ID, an invalid regular
// expression or a missing rubric refuses the whole file. A gate that silently drops the
// check someone just added reports a pass it never tested.
[[nodiscard]] bool ParseGoldenSet(const std::string& jsonLines, std::vector<GoldenCase>& outCases, std::string& outError);
[[nodiscard]] bool LoadGoldenSet(const std::filesystem::path& path, std::vector<GoldenCase>& outCases, std::string& outError);

// Returns the failure descriptions for one check, empty when it holds.
[[nodiscard]] std::vector<std::string> ApplyGoldenCheck(
    const GoldenCheck& check, const std::string& input, const std::string& reply, const std::vector<std::string>& earlierOpenings = {});
[[nodiscard]] std::vector<std::string> CheckGoldenTurn(
    const GoldenTurn& turn, const std::string& reply, const std::vector<std::string>& earlierOpenings = {});

// Numbers as written in a reply: grouping commas, decimals, signs, currency and percent.
[[nodiscard]] std::vector<double> ExtractNumbers(const std::string& text);
[[nodiscard]] std::size_t CountListItems(const std::string& text);
[[nodiscard]] std::size_t CountWords(const std::string& text);

// The same cases as conversation turns, so the existing evaluator collects the replies.
[[nodiscard]] EvaluationCase ToEvaluationCase(const GoldenCase& item);
}
