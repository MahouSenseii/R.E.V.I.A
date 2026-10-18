#include "Attachments/documentText.h"

#include "Attachments/zipArchive.h"
#include "Core/utf8.h"

#include <expat.h>

#include <memory>
#include <optional>
#include <type_traits>

namespace revia::attachments
{
namespace
{
// The XML part of a Word document is far larger than its text; this bounds what is
// inflated before parsing, separately from the text limit.
constexpr std::size_t MaximumDocxXmlBytes = 32 * 1024 * 1024;

void AppendUtf8(std::string& output, const char32_t codePoint)
{
    if (codePoint < 0x80)
    {
        output.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint < 0x800)
    {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint < 0x10000)
    {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else
    {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

std::string NormalizeNewlines(const std::string_view text)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] == '\r')
        {
            output.push_back('\n');
            if (index + 1 < text.size() && text[index + 1] == '\n')
                ++index;
            continue;
        }
        output.push_back(text[index]);
    }
    return output;
}

ExtractedText Refuse(std::string why)
{
    ExtractedText result;
    result.reason = std::move(why);
    return result;
}

ExtractedText Finish(std::string text, const std::size_t maxTextBytes, bool truncated = false)
{
    ExtractedText result;
    if (text.size() > maxTextBytes)
    {
        text = revia::utf8::Prefix(text, maxTextBytes);
        truncated = true;
    }
    result.ok = true;
    result.text = std::move(text);
    result.truncated = truncated;
    if (truncated)
        result.reason = "Only the first " + std::to_string(maxTextBytes / 1024) + " KB of text was kept.";
    return result;
}

std::optional<std::string> DecodeUtf16(const std::string_view bytes, const bool bigEndian)
{
    if (bytes.size() % 2 != 0)
        return std::nullopt;
    std::string output;
    output.reserve(bytes.size());
    const auto unit = [&](const std::size_t at)
    {
        const auto first = static_cast<unsigned char>(bytes[at]);
        const auto second = static_cast<unsigned char>(bytes[at + 1]);
        return static_cast<char32_t>(bigEndian ? (first << 8) | second : (second << 8) | first);
    };
    for (std::size_t at = 0; at < bytes.size(); at += 2)
    {
        char32_t codePoint = unit(at);
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
        {
            if (at + 3 >= bytes.size())
                return std::nullopt;
            const char32_t low = unit(at + 2);
            if (low < 0xDC00 || low > 0xDFFF)
                return std::nullopt;
            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
            at += 2;
        }
        else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
        {
            return std::nullopt;
        }
        if (codePoint == 0)
            return std::nullopt;
        AppendUtf8(output, codePoint);
    }
    return output;
}

struct DocxReader
{
    XML_Parser parser = nullptr;
    std::string text;
    std::size_t limit = 0;
    int insideText = 0;
    bool truncated = false;
    bool refused = false;

    static std::string_view LocalName(const XML_Char* name)
    {
        const std::string_view full(name);
        const std::size_t separator = full.rfind('|');
        return separator == std::string_view::npos ? full : full.substr(separator + 1);
    }

    void Append(const std::string_view value)
    {
        if (truncated)
            return;
        if (text.size() + value.size() > limit)
        {
            text += value.substr(0, limit - text.size());
            text = revia::utf8::Prefix(text, limit);
            truncated = true;
            XML_StopParser(parser, XML_FALSE);
            return;
        }
        text += value;
    }

    static void XMLCALL Start(void* data, const XML_Char* name, const XML_Char**)
    {
        auto& reader = *static_cast<DocxReader*>(data);
        const std::string_view local = LocalName(name);
        if (local == "t")
            ++reader.insideText;
        else if (local == "tab")
            reader.Append("\t");
        else if (local == "br" || local == "cr")
            reader.Append("\n");
    }

    static void XMLCALL End(void* data, const XML_Char* name)
    {
        auto& reader = *static_cast<DocxReader*>(data);
        const std::string_view local = LocalName(name);
        if (local == "t" && reader.insideText > 0)
            --reader.insideText;
        else if (local == "p" || local == "tr")
            reader.Append("\n");
        else if (local == "tc")
        {
            // A cell's last paragraph ends the cell, not the line: keep a row on one line.
            if (!reader.truncated && !reader.text.empty() && reader.text.back() == '\n')
                reader.text.pop_back();
            reader.Append("\t");
        }
    }

    static void XMLCALL Characters(void* data, const XML_Char* value, const int size)
    {
        auto& reader = *static_cast<DocxReader*>(data);
        if (reader.insideText > 0 && size > 0)
            reader.Append(std::string_view(value, static_cast<std::size_t>(size)));
    }

    // A document type declaration is how entity expansion attacks begin; Word never
    // writes one.
    static void XMLCALL Doctype(void* data, const XML_Char*, const XML_Char*, const XML_Char*, int)
    {
        auto& reader = *static_cast<DocxReader*>(data);
        reader.refused = true;
        XML_StopParser(reader.parser, XML_FALSE);
    }
};
} // namespace

ExtractedText DecodeTextFile(const std::string_view bytes, const std::size_t maxTextBytes)
{
    std::string decoded;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE)
    {
        const auto converted = DecodeUtf16(bytes.substr(2), false);
        if (!converted)
            return Refuse("The file says it is UTF-16 text but is not valid UTF-16.");
        decoded = *converted;
    }
    else if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF)
    {
        const auto converted = DecodeUtf16(bytes.substr(2), true);
        if (!converted)
            return Refuse("The file says it is UTF-16 text but is not valid UTF-16.");
        decoded = *converted;
    }
    else
    {
        std::string_view body = bytes;
        if (body.size() >= 3 && body.substr(0, 3) == "\xEF\xBB\xBF")
            body.remove_prefix(3);
        if (body.find('\0') != std::string_view::npos || !revia::utf8::IsValid(body))
            return Refuse("The file is not UTF-8 text. Binary files and other text encodings are not supported.");
        decoded.assign(body);
    }
    return Finish(NormalizeNewlines(decoded), maxTextBytes);
}

ExtractedText ExtractDocxText(const std::string_view bytes, const std::size_t maxTextBytes)
{
    const ZipEntry document = ReadZipEntry(bytes, "word/document.xml", MaximumDocxXmlBytes);
    if (!document.ok)
        return Refuse(document.found ? "The Word document could not be read: " + document.error
                                     : "This is not a Word (.docx) document: " + document.error);

    std::unique_ptr<std::remove_pointer_t<XML_Parser>, decltype(&XML_ParserFree)> parser(XML_ParserCreateNS("UTF-8", '|'), &XML_ParserFree);
    if (!parser)
        return Refuse("The XML parser could not be created.");
    DocxReader reader;
    reader.parser = parser.get();
    reader.limit = maxTextBytes;
    XML_SetUserData(parser.get(), &reader);
    XML_SetElementHandler(parser.get(), DocxReader::Start, DocxReader::End);
    XML_SetCharacterDataHandler(parser.get(), DocxReader::Characters);
    XML_SetStartDoctypeDeclHandler(parser.get(), DocxReader::Doctype);
    XML_SetParamEntityParsing(parser.get(), XML_PARAM_ENTITY_PARSING_NEVER);
    XML_SetExternalEntityRefHandler(parser.get(),
        [](XML_Parser, const XML_Char*, const XML_Char*, const XML_Char*, const XML_Char*) -> int { return XML_STATUS_ERROR; });
    const XML_Status status = XML_Parse(parser.get(), document.data.data(), static_cast<int>(document.data.size()), XML_TRUE);
    if (reader.refused)
        return Refuse("The Word document declares a document type, which Word never writes; it was not read.");
    if (status != XML_STATUS_OK && !reader.truncated)
        return Refuse(std::string("The Word document's XML is malformed: ") + XML_ErrorString(XML_GetErrorCode(parser.get())));
    if (!revia::utf8::IsValid(reader.text))
        return Refuse("The Word document's text is not valid UTF-8.");
    return Finish(NormalizeNewlines(reader.text), maxTextBytes, reader.truncated);
}

} // namespace revia::attachments
