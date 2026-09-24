#include "Visual/svgCanvas.h"

#include <expat.h>
#include <algorithm>
#include <cctype>
#include <memory>
#include <set>
#include <string_view>
#include <type_traits>

namespace revia::visual
{
namespace
{
constexpr std::string_view SvgNamespace = "http://www.w3.org/2000/svg";
constexpr std::string_view XlinkNamespace = "http://www.w3.org/1999/xlink";
constexpr char NamespaceSeparator = '\x1f';

std::string Lower(std::string value)
{
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string Trim(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\n\r");
    return first == std::string::npos ? std::string{} :
        value.substr(first, value.find_last_not_of(" \t\n\r") - first + 1);
}

std::pair<std::string, std::string> ExpandedName(const char* name)
{
    const std::string expanded(name);
    const auto separator = expanded.find(NamespaceSeparator);
    return separator == std::string::npos ? std::pair{std::string{}, expanded} :
        std::pair{expanded.substr(0, separator), expanded.substr(separator + 1)};
}

bool LocalReference(const std::string& value)
{
    return value.size() > 1 && value.front() == '#' &&
        std::all_of(value.begin() + 1, value.end(), [](const unsigned char c)
        { return std::isalnum(c) || c >= 0x80 || c == '_' || c == '-' || c == '.' || c == ':'; });
}

const std::set<std::string> PresentationAttributes = {
    "fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity",
    "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray",
    "stroke-dashoffset", "opacity", "color", "font-family", "font-size", "font-style",
    "font-weight", "font-variant", "font-stretch", "text-anchor", "text-decoration",
    "dominant-baseline", "alignment-baseline", "baseline-shift", "letter-spacing",
    "word-spacing", "direction", "unicode-bidi", "clip-path", "clip-rule", "mask",
    "marker-start", "marker-mid", "marker-end", "stop-color", "stop-opacity",
    "display", "visibility", "overflow", "vector-effect", "paint-order",
    "shape-rendering", "text-rendering", "color-interpolation"
};

// CSS escapes, comments, at-rules and arbitrary functions are outside our drawing
// vocabulary. XML has already decoded entities before any value reaches this check.
bool DrawingValue(const std::string& value)
{
    if (value.find_first_of("\\@{}<>;") != std::string::npos ||
        value.find("/*") != std::string::npos || value.find("*/") != std::string::npos)
        return false;
    for (const unsigned char c : value)
        if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') return false;
    const auto lower = Lower(value);
    static const std::set<std::string> functions = {
        "rgb", "rgba", "hsl", "hsla", "matrix", "translate", "translatex", "translatey",
        "scale", "scalex", "scaley", "rotate", "skewx", "skewy"
    };
    for (std::size_t at = 0; at < lower.size(); ++at)
    {
        if (lower[at] == ')') return false;
        if (lower[at] != '(') continue;
        std::size_t start = at;
        while (start > 0 && std::isalpha(static_cast<unsigned char>(lower[start - 1]))) --start;
        const auto function = lower.substr(start, at - start);
        const auto close = lower.find(')', at + 1);
        if (close == std::string::npos || lower.find('(', at + 1) < close) return false;
        auto argument = Trim(value.substr(at + 1, close - at - 1));
        if (function == "url")
        {
            if (argument.size() >= 2 && (argument.front() == '\'' || argument.front() == '"') &&
                argument.back() == argument.front()) argument = argument.substr(1, argument.size() - 2);
            if (!LocalReference(argument)) return false;
        }
        else if (!functions.contains(function)) return false;
        at = close;
    }
    return true;
}

bool DrawingStyle(const std::string& style)
{
    std::size_t start = 0;
    while (start < style.size())
    {
        const auto end = style.find(';', start);
        const auto declaration = Trim(style.substr(start, end - start));
        if (!declaration.empty())
        {
            const auto colon = declaration.find(':');
            if (colon == std::string::npos ||
                !PresentationAttributes.contains(Lower(Trim(declaration.substr(0, colon)))) ||
                !DrawingValue(Trim(declaration.substr(colon + 1)))) return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

std::string Escape(const std::string_view text)
{
    std::string result;
    for (const char c : text)
    {
        switch (c)
        {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\r': result += "&#13;"; break;
        case '\n': result += "&#10;"; break;
        case '\t': result += "&#9;"; break;
        default: result += c;
        }
    }
    return result;
}

struct SvgReader
{
    XML_Parser parser = nullptr;
    std::string output;
    std::string error;
    std::string rootNamespace;
    std::size_t depth = 0;
    std::size_t nodes = 0;

    void Refuse(const std::string& reason)
    {
        if (error.empty()) error = reason;
        XML_StopParser(parser, XML_FALSE);
    }

    void Append(const std::string& text)
    {
        if (output.size() + text.size() > SvgSanitizer::MaximumCharacters)
            Refuse("serialized SVG exceeds the size ceiling");
        else output += text;
    }

    static void XMLCALL Start(void* data, const XML_Char* name, const XML_Char** attributes)
    {
        auto& self = *static_cast<SvgReader*>(data);
        const auto [uri, local] = ExpandedName(name);
        static const std::set<std::string> elements = {
            "svg", "g", "defs", "title", "desc", "metadata", "path", "rect", "circle",
            "ellipse", "line", "polyline", "polygon", "text", "tspan", "textPath",
            "marker", "symbol", "use", "linearGradient", "radialGradient", "stop",
            "clipPath", "mask", "pattern"
        };
        static const std::set<std::string> geometry = {
            "id", "version", "viewBox", "preserveAspectRatio", "x", "y", "x1", "y1",
            "x2", "y2", "cx", "cy", "r", "rx", "ry", "width", "height", "d", "points",
            "transform", "dx", "dy", "rotate", "textLength", "lengthAdjust", "startOffset",
            "method", "spacing", "offset", "gradientUnits", "gradientTransform",
            "spreadMethod", "fx", "fy", "fr", "patternUnits", "patternContentUnits",
            "patternTransform", "clipPathUnits", "maskUnits", "maskContentUnits",
            "markerUnits", "markerWidth", "markerHeight", "refX", "refY", "orient", "pathLength"
        };
        if (self.depth == 0)
        {
            if (local != "svg" || (!uri.empty() && uri != SvgNamespace))
            { self.Refuse("the root is not an SVG element"); return; }
            self.rootNamespace = uri;
        }
        if (uri != self.rootNamespace || !elements.contains(local))
        { self.Refuse("unsupported element or namespace: " + local); return; }
        if (++self.depth > 128 || ++self.nodes > 20000)
        { self.Refuse("SVG nesting or element limit exceeded"); return; }
        self.Append("<" + local);
        if (self.depth == 1)
            self.Append(" xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\"");
        for (std::size_t at = 0; attributes[at]; at += 2)
        {
            if (at >= 128) { self.Refuse("too many SVG attributes"); return; }
            const auto [attributeUri, attribute] = ExpandedName(attributes[at]);
            const std::string value(attributes[at + 1]);
            bool valid = false;
            std::string serializedName = attribute;
            if (attribute == "href" && (attributeUri.empty() || attributeUri == XlinkNamespace))
            {
                valid = LocalReference(value);
                if (!attributeUri.empty()) serializedName = "xlink:href";
            }
            else if (attributeUri == "http://www.w3.org/XML/1998/namespace" && attribute == "space")
            { valid = value == "default" || value == "preserve"; serializedName = "xml:space"; }
            else if (attributeUri.empty())
            {
                valid = attribute == "style" ? DrawingStyle(value) :
                    (geometry.contains(attribute) || PresentationAttributes.contains(attribute)) && DrawingValue(value);
            }
            if (!valid) { self.Refuse("unsupported attribute or value: " + attribute); return; }
            self.Append(" " + serializedName + "=\"" + Escape(value) + "\"");
        }
        self.Append(">");
    }

    static void XMLCALL End(void* data, const XML_Char* name)
    {
        auto& self = *static_cast<SvgReader*>(data);
        self.Append("</" + ExpandedName(name).second + ">");
        --self.depth;
    }
    static void XMLCALL Text(void* data, const XML_Char* text, const int size)
    { static_cast<SvgReader*>(data)->Append(Escape(std::string_view(text, static_cast<std::size_t>(size)))); }
    static void XMLCALL Doctype(void* data, const XML_Char*, const XML_Char*, const XML_Char*, int)
    { static_cast<SvgReader*>(data)->Refuse("DTD declarations are not permitted"); }
    static void XMLCALL Instruction(void* data, const XML_Char*, const XML_Char*)
    { static_cast<SvgReader*>(data)->Refuse("processing instructions are not permitted"); }
};
}

SvgValidation SvgSanitizer::Sanitize(const std::string& markup)
{
    SvgValidation result;
    const auto refuse = [&](const std::string& reason)
    {
        result.removed.push_back(reason);
        result.reason = "The diagram was refused: " + reason + ".";
        return result;
    };
    // Bound wrapper prose too, before allocating a lowercase copy or parsing XML.
    if (markup.size() > MaximumCharacters + 16 * 1024)
        return refuse("the diagram response exceeds the size ceiling");
    const auto lower = Lower(markup);
    if (lower.find("<!doctype") != std::string::npos || lower.find("<!entity") != std::string::npos)
        return refuse("entity and doctype declarations are not permitted");
    const auto extracted = ExtractSvg(markup);
    if (extracted.empty()) return refuse("no complete SVG element was found");
    if (extracted.size() > MaximumCharacters) return refuse("SVG exceeds the size ceiling");

    std::unique_ptr<std::remove_pointer_t<XML_Parser>, decltype(&XML_ParserFree)>
        parser(XML_ParserCreateNS("UTF-8", NamespaceSeparator), &XML_ParserFree);
    if (!parser) return refuse("XML parser allocation failed");
    SvgReader reader;
    reader.parser = parser.get();
    XML_SetUserData(parser.get(), &reader);
    XML_SetElementHandler(parser.get(), SvgReader::Start, SvgReader::End);
    XML_SetCharacterDataHandler(parser.get(), SvgReader::Text);
    XML_SetStartDoctypeDeclHandler(parser.get(), SvgReader::Doctype);
    XML_SetProcessingInstructionHandler(parser.get(), SvgReader::Instruction);
    XML_SetParamEntityParsing(parser.get(), XML_PARAM_ENTITY_PARSING_NEVER);
    XML_SetExternalEntityRefHandler(parser.get(), [](XML_Parser, const XML_Char*, const XML_Char*,
        const XML_Char*, const XML_Char*) -> int { return XML_STATUS_ERROR; });
    if (XML_Parse(parser.get(), extracted.data(), static_cast<int>(extracted.size()), XML_TRUE) != XML_STATUS_OK)
        return refuse(reader.error.empty() ? XML_ErrorString(XML_GetErrorCode(parser.get())) : reader.error);
    if (!reader.error.empty()) return refuse(reader.error);
    result.accepted = true;
    result.markup = std::move(reader.output);
    result.reason = "Accepted: parsed drawing vocabulary with only local references.";
    return result;
}
}
