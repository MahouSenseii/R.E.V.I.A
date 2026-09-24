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
