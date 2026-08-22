#include "Visual/svgCanvas.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace revia::visual
{

namespace
{

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string UtcTimestamp()
{
    const std::time_t time = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H-%M-%SZ");
    return stream.str();
}

std::string Slug(const std::string& title)
{
    std::string slug;
    for (const unsigned char character : title)
    {
        if (std::isalnum(character))
        {
            slug.push_back(static_cast<char>(std::tolower(character)));
        }
        else if (!slug.empty() && slug.back() != '-')
        {
            slug.push_back('-');
        }
        if (slug.size() >= 48)
        {
            break;
        }
    }
    while (!slug.empty() && slug.back() == '-')
    {
        slug.pop_back();
    }
    return slug.empty() ? "diagram" : slug;
}

// Elements whose presence means the document is doing something other than drawing.
const std::vector<std::string>& ForbiddenElements()
{
    static const std::vector<std::string> elements = {
        "script", "foreignobject", "iframe", "embed", "object", "handler", "audio",
        "video", "animate", "set"
    };
    return elements;
}

// Attribute prefixes and values that reach outside the document.
const std::vector<std::string>& ForbiddenAttributePatterns()
{
    static const std::vector<std::string> patterns = {
        "onload", "onclick", "onmouseover", "onmouseout", "onerror", "onbegin",
        "onend", "onrepeat", "onactivate", "onfocusin", "onfocusout",
        "javascript:", "data:text/html", "xlink:href=\"http", "xlink:href='http",
        "href=\"http", "href='http", "href=\"file", "href='file",
        "xlink:href=\"file", "xlink:href='file"
    };
    return patterns;
}

} // namespace

std::string SvgSanitizer::ExtractSvg(const std::string& response)
{
    const std::string lowered = Lower(response);
    const std::size_t start = lowered.find("<svg");
    if (start == std::string::npos)
    {
        return {};
    }
    const std::size_t close = lowered.rfind("</svg>");
    if (close == std::string::npos || close < start)
    {
        return {};
    }
    return response.substr(start, close + std::string("</svg>").size() - start);
}

SvgValidation SvgSanitizer::Sanitize(const std::string& markup)
{
    SvgValidation validation;

    // Checked against the whole response rather than the extracted element, because a
    // doctype sits *before* <svg> and extraction would otherwise drop it silently. A
    // dropped entity declaration is not a safe one: it means something upstream tried to
    // read a local file into the picture, and that is worth refusing outright rather than
    // quietly rendering whatever survived.
    const std::string wholeResponse = Lower(markup);
    if (wholeResponse.find("<!entity") != std::string::npos ||
        wholeResponse.find("<!doctype") != std::string::npos)
    {
        validation.removed.emplace_back("an entity or doctype declaration");
        validation.reason =
            "The diagram was refused because it contains an entity or doctype "
            "declaration. A diagram draws; it does not read files.";
        return validation;
    }

    const std::string extracted = ExtractSvg(markup);
    if (extracted.empty())
    {
        validation.reason = "No <svg> element was found in the response.";
        return validation;
    }
    if (extracted.size() > MaximumCharacters)
    {
        validation.reason = "The diagram is larger than the " +
            std::to_string(MaximumCharacters / 1024) + " KiB ceiling.";
        return validation;
    }

    const std::string lowered = Lower(extracted);

    // Refused rather than stripped. Removing a <script> and rendering the rest assumes
    // the rest was written in good faith by something that also wrote a script.
    for (const std::string& element : ForbiddenElements())
    {
        if (lowered.find("<" + element) != std::string::npos)
        {
            validation.removed.push_back("<" + element + ">");
        }
    }
    for (const std::string& pattern : ForbiddenAttributePatterns())
    {
        if (lowered.find(pattern) != std::string::npos)
        {
            validation.removed.push_back(pattern);
        }
    }
    if (!validation.removed.empty())
    {
        std::ostringstream reason;
        reason << "The diagram was refused because it contains";
        for (std::size_t index = 0; index < validation.removed.size(); ++index)
        {
            reason << (index == 0 ? " " : ", ") << validation.removed[index];
        }
        reason << ". A diagram draws; it does not run code or fetch anything.";
        validation.reason = reason.str();
        return validation;
    }

    validation.accepted = true;
    validation.markup = extracted;
    validation.reason = "Accepted: no script, no external references, no embedded documents.";
    return validation;
}

DiagramStore::DiagramStore(std::filesystem::path inputRoot)
    : root(std::move(inputRoot))
{
}

std::filesystem::path DiagramStore::Root() const
{
    return root;
}

bool DiagramStore::Save(
    const std::string& title,
    const std::string& markup,
    Diagram& outDiagram,
    std::string& outError) const
{
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error)
    {
        outError = "Could not create " + root.string() + ": " + error.message();
        return false;
    }

    const std::string stamp = UtcTimestamp();
    outDiagram.id = stamp + "-" + Slug(title);
    outDiagram.title = title.empty() ? "Untitled diagram" : title;
    outDiagram.markup = markup;
    outDiagram.createdAt = stamp;
    outDiagram.path = root / (outDiagram.id + ".svg");

    std::ofstream file(outDiagram.path, std::ios::trunc | std::ios::binary);
    if (!file.is_open())
    {
        outError = "Could not write " + outDiagram.path.string() + '.';
        return false;
    }
    file << markup;
    if (!file.good())
    {
        outError = "The diagram could not be written completely.";
        return false;
    }
    return true;
}

std::vector<Diagram> DiagramStore::Recent(const std::size_t maxDiagrams) const
{
    std::vector<Diagram> diagrams;
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error)
    {
        return diagrams;
    }
    for (const std::filesystem::directory_entry& entry :
        std::filesystem::directory_iterator(root, error))
    {
        if (error)
        {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".svg")
        {
            continue;
        }
        Diagram diagram;
        diagram.id = entry.path().stem().string();
        diagram.title = diagram.id;
        diagram.path = entry.path();
        diagrams.push_back(std::move(diagram));
    }
    std::sort(diagrams.begin(), diagrams.end(),
        [](const Diagram& left, const Diagram& right)
        {
            return left.id > right.id;
        });
    if (diagrams.size() > maxDiagrams)
    {
        diagrams.resize(maxDiagrams);
    }
    return diagrams;
}

} // namespace revia::visual
