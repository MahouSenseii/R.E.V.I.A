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
    // What was stripped, so a refusal or a cleanup is explainable rather than silent.
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

// Makes model-produced SVG safe to render, or refuses it.
//
// The model writes this markup, so it is untrusted input in the same sense a web page is.
// Qt's renderer does not execute script, but "the current renderer happens not to" is not
// a security property -- it is a version. The sanitizer enforces the property directly:
//
//   - No script, no event handlers. Nothing in a diagram should run.
//   - No external references. A diagram that fetches a URL turns a drawing into a network
//     request and a tracking pixel, from markup the user never wrote.
//   - No foreignObject. It embeds an arbitrary other document inside the picture, which
//     is the one element that would reopen everything above.
//   - No entity declarations, which are the classic route to reading local files.
//
// A document that needs any of those is refused rather than repaired, because a drawing
// that insists on running code is not a drawing.
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

    bool Save(
        const std::string& title,
        const std::string& markup,
        Diagram& outDiagram,
        std::string& outError) const;
    [[nodiscard]] std::vector<Diagram> Recent(std::size_t maxDiagrams = 20) const;
    [[nodiscard]] std::filesystem::path Root() const;

private:
    std::filesystem::path root;
};

} // namespace revia::visual
