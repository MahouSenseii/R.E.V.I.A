#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace revia::visual
{

struct SvgValidation
{
    bool accepted = false;
    std::string reason;
    // Refused constructs or parser errors. Rejected markup is never returned.
    std::vector<std::string> removed;
    std::string markup;
};

struct Diagram
{
    std::string id;
    std::string title;
    std::string markup;
    std::string createdAt;
    std::filesystem::path path;
};

// Sanitizes untrusted SVG with namespace-aware parsing, decoded-value checks and canonical serialization.
// Rejects scripts, event handlers, external references, foreignObject and entity declarations.
// Only drawing attributes and a restricted inline-style vocabulary survive; unsafe documents are refused.
class SvgSanitizer
{
public:
    // Hard ceiling. A diagram is a picture, not a payload, and an unbounded one costs
    // memory in the renderer before anything else gets a say.
    static constexpr std::size_t MaximumCharacters = 512 * 1024;

    [[nodiscard]] static SvgValidation Sanitize(const std::string& markup);
    // Pulls the SVG out of whatever the model wrapped it in -- a fenced code block, a
    // JSON string, or prose either side of it.
    [[nodiscard]] static std::string ExtractSvg(const std::string& response);
};

// Where accepted diagrams live. Files rather than a database: a diagram is something the
// user will want to open in another program, and a folder is the interface for that.
class DiagramStore
{
public:
    explicit DiagramStore(std::filesystem::path root = "RuntimeData/Diagrams");

    bool Save(const std::string& title, const std::string& markup, Diagram& outDiagram, std::string& outError) const;
    [[nodiscard]] std::vector<Diagram> Recent(std::size_t maxDiagrams = 20) const;
    [[nodiscard]] std::filesystem::path Root() const;

private:
    std::filesystem::path root;
};

} // namespace revia::visual
