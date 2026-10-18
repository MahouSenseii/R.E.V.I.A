#include "Attachments/documentText.h"

#include "Attachments/inflate.h"
#include "Core/utf8.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace revia::attachments
{
namespace
{
// Every limit here is about a hostile file, not a large one: an ordinary 300-page PDF
// stays far inside all of them.
constexpr std::size_t MaximumObjects = 200000;
constexpr std::size_t MaximumStreamBytes = 16 * 1024 * 1024;
constexpr std::size_t MaximumDecodedTotal = 160 * 1024 * 1024;
constexpr std::size_t MaximumPages = 5000;
constexpr std::size_t MaximumContainerItems = 100000;
constexpr std::size_t MaximumStringBytes = 1024 * 1024;
constexpr int MaximumNesting = 32;
constexpr int MaximumFormDepth = 8;
constexpr std::size_t MaximumCMapEntries = 1 << 20;

bool IsWhitespace(const char character)
{
    return character == ' ' || character == '\n' || character == '\r' || character == '\t' || character == '\f' || character == '\0';
}

bool IsDelimiter(const char character)
{
    return character == '(' || character == ')' || character == '<' || character == '>' || character == '[' || character == ']' ||
           character == '{' || character == '}' || character == '/' || character == '%';
}

bool IsRegular(const char character)
{
    return !IsWhitespace(character) && !IsDelimiter(character);
}

int HexValue(const char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

enum class TokenKind
{
    End,
    Number,
    Name,
    String,
    ArrayOpen,
    ArrayClose,
    DictOpen,
    DictClose,
    Keyword
};

struct Token
{
    TokenKind kind = TokenKind::End;
    std::string text;
    double number = 0.0;
    bool integer = false;
};

// The PDF lexer. Shared by object bodies, content streams and ToUnicode CMaps, which
// all use the same token syntax.
class Lexer
{
public:
    explicit Lexer(const std::string_view source, const std::size_t start = 0) : data(source), position(start) {}

    [[nodiscard]] std::size_t Position() const { return position; }
    void Seek(const std::size_t to) { position = std::min(to, data.size()); }
    [[nodiscard]] std::string_view Data() const { return data; }

    void SkipSpace()
    {
        while (position < data.size())
        {
            if (IsWhitespace(data[position]))
            {
                ++position;
            }
            else if (data[position] == '%')
            {
                while (position < data.size() && data[position] != '\n' && data[position] != '\r')
                    ++position;
            }
            else
            {
                break;
            }
        }
    }

    Token Next()
    {
        SkipSpace();
        Token token;
        if (position >= data.size())
            return token;
        const char character = data[position];
        if (character == '(')
        {
            token.kind = TokenKind::String;
            token.text = ReadLiteral();
            return token;
        }
        if (character == '<')
        {
            if (position + 1 < data.size() && data[position + 1] == '<')
            {
                position += 2;
                token.kind = TokenKind::DictOpen;
                return token;
            }
            token.kind = TokenKind::String;
            token.text = ReadHex();
            return token;
        }
        if (character == '>')
        {
            position += position + 1 < data.size() && data[position + 1] == '>' ? 2 : 1;
            token.kind = TokenKind::DictClose;
            return token;
        }
        if (character == '[' || character == ']')
        {
            ++position;
            token.kind = character == '[' ? TokenKind::ArrayOpen : TokenKind::ArrayClose;
            return token;
        }
        if (character == '{' || character == '}' || character == ')')
        {
            ++position;
            token.kind = TokenKind::Keyword;
            token.text.assign(1, character);
            return token;
        }
        if (character == '/')
        {
            ++position;
            token.kind = TokenKind::Name;
            while (position < data.size() && IsRegular(data[position]))
            {
                if (data[position] == '#' && position + 2 < data.size() && HexValue(data[position + 1]) >= 0 &&
                    HexValue(data[position + 2]) >= 0)
                {
                    token.text.push_back(static_cast<char>(HexValue(data[position + 1]) * 16 + HexValue(data[position + 2])));
                    position += 3;
                    continue;
                }
                token.text.push_back(data[position++]);
            }
            return token;
        }
        const std::size_t start = position;
        while (position < data.size() && IsRegular(data[position]))
            ++position;
        if (position == start)
        {
            // An unexpected delimiter. Step over it so the caller always makes progress.
            ++position;
            token.kind = TokenKind::Keyword;
            return token;
        }
        token.text.assign(data.substr(start, position - start));
        if (ParseNumber(token.text, token.number, token.integer))
            token.kind = TokenKind::Number;
        else
            token.kind = TokenKind::Keyword;
        return token;
    }

private:
    static bool ParseNumber(const std::string& text, double& value, bool& integer)
    {
        std::size_t index = 0;
        bool negative = false;
        if (index < text.size() && (text[index] == '+' || text[index] == '-'))
            negative = text[index++] == '-';
        bool digits = false;
        bool dot = false;
        double whole = 0.0;
        double scale = 1.0;
        for (; index < text.size(); ++index)
        {
            const char character = text[index];
            if (character >= '0' && character <= '9')
            {
                digits = true;
                if (dot)
                {
                    scale /= 10.0;
                    whole += (character - '0') * scale;
                }
                else
                {
                    whole = whole * 10.0 + (character - '0');
                }
            }
            else if (character == '.' && !dot)
            {
                dot = true;
            }
            else
            {
                return false;
            }
        }
        if (!digits)
            return false;
        value = negative ? -whole : whole;
        integer = !dot;
        return true;
    }

    std::string ReadLiteral()
    {
        std::string text;
        int depth = 0;
        ++position;
        while (position < data.size())
        {
            const char character = data[position++];
            if (character == '\\')
            {
                if (position >= data.size())
                    break;
                const char escaped = data[position++];
                switch (escaped)
                {
                    case 'n': text.push_back('\n'); break;
                    case 'r': text.push_back('\r'); break;
                    case 't': text.push_back('\t'); break;
                    case 'b': text.push_back('\b'); break;
                    case 'f': text.push_back('\f'); break;
                    case '\r':
                        if (position < data.size() && data[position] == '\n')
                            ++position;
                        break;
                    case '\n': break;
                    default:
                        if (escaped >= '0' && escaped <= '7')
                        {
                            int value = escaped - '0';
                            for (int digit = 0; digit < 2 && position < data.size() && data[position] >= '0' && data[position] <= '7';
                                 ++digit)
                                value = value * 8 + (data[position++] - '0');
                            text.push_back(static_cast<char>(value & 0xFF));
                        }
                        else
                        {
                            text.push_back(escaped);
                        }
                }
            }
            else if (character == '(')
            {
                ++depth;
                text.push_back(character);
            }
            else if (character == ')')
            {
                if (depth == 0)
                    break;
                --depth;
                text.push_back(character);
            }
            else
            {
                text.push_back(character);
            }
            if (text.size() > MaximumStringBytes)
                break;
        }
        return text;
    }

    std::string ReadHex()
    {
        std::string text;
        ++position;
        int high = -1;
        while (position < data.size() && data[position] != '>')
        {
            const int value = HexValue(data[position++]);
            if (value < 0)
                continue;
            if (high < 0)
            {
                high = value;
            }
            else
            {
                text.push_back(static_cast<char>(high * 16 + value));
                high = -1;
            }
            if (text.size() > MaximumStringBytes)
                break;
        }
        if (high >= 0)
            text.push_back(static_cast<char>(high * 16));
        if (position < data.size())
            ++position;
        return text;
    }

    std::string_view data;
    std::size_t position = 0;
};

struct Value
{
    enum class Kind
    {
        Null,
        Number,
        Name,
        String,
        Reference,
        Array,
        Dictionary,
        Keyword
    };
    Kind kind = Kind::Null;
    double number = 0.0;
    std::string text;
    int reference = 0;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> entries;

    [[nodiscard]] const Value* Get(const std::string_view key) const
    {
        for (const auto& [name, value] : entries)
            if (name == key)
                return &value;
        return nullptr;
    }
    [[nodiscard]] bool IsName(const std::string_view name) const { return kind == Kind::Name && text == name; }
};

// One value at the lexer's position. Two numbers followed by R are an indirect reference.
std::optional<Value> ParseValue(Lexer& lexer, const int depth = 0)
{
    if (depth > MaximumNesting)
        return std::nullopt;
    const Token token = lexer.Next();
    Value value;
    switch (token.kind)
    {
        case TokenKind::End:
        case TokenKind::ArrayClose:
        case TokenKind::DictClose:
            return std::nullopt;
        case TokenKind::Number:
        {
            value.kind = Value::Kind::Number;
            value.number = token.number;
            if (token.integer && token.number >= 0)
            {
                const std::size_t saved = lexer.Position();
                const Token generation = lexer.Next();
                if (generation.kind == TokenKind::Number && generation.integer)
                {
                    const Token marker = lexer.Next();
                    if (marker.kind == TokenKind::Keyword && marker.text == "R")
                    {
                        value.kind = Value::Kind::Reference;
                        value.reference = static_cast<int>(std::min(token.number, 2147483647.0));
                        return value;
                    }
                }
                lexer.Seek(saved);
            }
            return value;
        }
        case TokenKind::Name:
            value.kind = Value::Kind::Name;
            value.text = token.text;
            return value;
        case TokenKind::String:
            value.kind = Value::Kind::String;
            value.text = token.text;
            return value;
        case TokenKind::Keyword:
            value.kind = token.text == "null" ? Value::Kind::Null : Value::Kind::Keyword;
            value.text = token.text;
            return value;
        case TokenKind::ArrayOpen:
        {
            value.kind = Value::Kind::Array;
            for (;;)
            {
                const std::size_t saved = lexer.Position();
                const Token peek = lexer.Next();
                if (peek.kind == TokenKind::ArrayClose || peek.kind == TokenKind::End)
                    return value;
                lexer.Seek(saved);
                auto item = ParseValue(lexer, depth + 1);
                if (!item || value.items.size() >= MaximumContainerItems)
                    return value;
                value.items.push_back(std::move(*item));
            }
        }
        case TokenKind::DictOpen:
        {
            value.kind = Value::Kind::Dictionary;
            for (;;)
            {
                const Token key = lexer.Next();
                if (key.kind == TokenKind::DictClose || key.kind == TokenKind::End)
                    return value;
                if (key.kind != TokenKind::Name)
                    continue;
                auto item = ParseValue(lexer, depth + 1);
                if (!item || value.entries.size() >= MaximumContainerItems)
                    return value;
                value.entries.emplace_back(key.text, std::move(*item));
            }
        }
    }
    return std::nullopt;
}

struct PdfObject
{
    Value value;
    bool hasStream = false;
    std::string_view stream;
};

// WinAnsiEncoding for 0x80-0x9F; the rest of the upper half is Latin-1.
constexpr std::array<char16_t, 32> WinAnsiHigh = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,
    0x017E, 0x0178};

void AppendCodePoint(std::string& output, const char32_t codePoint)
{
    if (codePoint == 0 || (codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF)
        return;
    if (codePoint < 0x20 && codePoint != '\t' && codePoint != '\n')
        return;
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

void AppendWinAnsi(std::string& output, const unsigned char byte)
{
    if (byte >= 0x80 && byte < 0xA0)
    {
        if (const char16_t mapped = WinAnsiHigh[byte - 0x80]; mapped != 0)
            AppendCodePoint(output, mapped);
        return;
    }
    AppendCodePoint(output, byte);
}

// UTF-16BE bytes, as a CMap destination is written, to UTF-8.
std::string Utf16BeToUtf8(const std::string_view bytes)
{
    std::string output;
    for (std::size_t at = 0; at + 1 < bytes.size(); at += 2)
    {
        char32_t unit = (static_cast<unsigned char>(bytes[at]) << 8) | static_cast<unsigned char>(bytes[at + 1]);
        if (unit >= 0xD800 && unit <= 0xDBFF && at + 3 < bytes.size())
        {
            const char32_t low = (static_cast<unsigned char>(bytes[at + 2]) << 8) | static_cast<unsigned char>(bytes[at + 3]);
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                at += 2;
            }
        }
        AppendCodePoint(output, unit);
    }
    return output;
}

std::uint32_t CodeValue(const std::string_view bytes)
{
    std::uint32_t value = 0;
    for (const char byte : bytes.substr(0, 4))
        value = (value << 8) | static_cast<unsigned char>(byte);
    return value;
}

// A ToUnicode CMap: which byte sequences are codes, and the text each code stands for.
struct ToUnicodeMap
{
    struct Range
    {
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        std::size_t length = 1;
        // Either a start value incremented across the range, or one destination per code.
        std::string start;
        std::vector<std::string> each;
    };
    std::vector<std::pair<std::size_t, std::pair<std::uint32_t, std::uint32_t>>> codespace;
    std::unordered_map<std::uint64_t, std::string> single;
    std::vector<Range> ranges;
    std::size_t entries = 0;

    [[nodiscard]] std::optional<std::string> Lookup(const std::uint32_t code, const std::size_t length) const
    {
        if (const auto found = single.find((static_cast<std::uint64_t>(length) << 32) | code); found != single.end())
            return found->second;
        for (const Range& range : ranges)
        {
            if (range.length != length || code < range.low || code > range.high)
                continue;
            const std::uint32_t offset = code - range.low;
            if (!range.each.empty())
                return offset < range.each.size() ? std::optional<std::string>(Utf16BeToUtf8(range.each[offset])) : std::nullopt;
            std::string destination = range.start;
            if (destination.size() >= 2)
            {
                // The last UTF-16 unit is incremented; ranges never cross a unit boundary.
                std::uint32_t last = (static_cast<unsigned char>(destination[destination.size() - 2]) << 8) |
                                     static_cast<unsigned char>(destination.back());
                last += offset;
                destination[destination.size() - 2] = static_cast<char>((last >> 8) & 0xFF);
                destination.back() = static_cast<char>(last & 0xFF);
            }
            return Utf16BeToUtf8(destination);
        }
        return std::nullopt;
    }

    // The byte length of the code starting at `bytes`, from the codespace ranges.
    [[nodiscard]] std::size_t CodeLength(const std::string_view bytes, const std::size_t fallback) const
    {
        for (std::size_t length = 1; length <= 4 && length <= bytes.size(); ++length)
        {
            const std::uint32_t code = CodeValue(bytes.substr(0, length));
            for (const auto& [rangeLength, bounds] : codespace)
                if (rangeLength == length && code >= bounds.first && code <= bounds.second)
                    return length;
        }
        return std::min(fallback, bytes.size());
    }
};

ToUnicodeMap ParseToUnicode(const std::string_view source)
{
    ToUnicodeMap map;
    Lexer lexer(source);
    std::vector<Token> operands;
    std::string section;
    for (Token token = lexer.Next(); token.kind != TokenKind::End; token = lexer.Next())
    {
        if (map.entries > MaximumCMapEntries)
            break;
        if (token.kind == TokenKind::Keyword)
        {
            if (token.text == "begincodespacerange" || token.text == "beginbfchar" || token.text == "beginbfrange")
            {
                section = token.text;
                operands.clear();
            }
            else if (token.text == "endcodespacerange" || token.text == "endbfchar")
            {
                const bool codespace = token.text == "endcodespacerange";
                for (std::size_t at = 0; at + 1 < operands.size(); at += 2)
                {
                    const std::string& first = operands[at].text;
                    const std::string& second = operands[at + 1].text;
                    if (first.empty() || first.size() > 4)
                        continue;
                    if (codespace)
                        map.codespace.push_back({first.size(), {CodeValue(first), CodeValue(second)}});
                    else
                        map.single[(static_cast<std::uint64_t>(first.size()) << 32) | CodeValue(first)] = Utf16BeToUtf8(second);
                    ++map.entries;
                }
                section.clear();
                operands.clear();
            }
            else if (token.text == "endbfrange")
            {
                section.clear();
                operands.clear();
            }
            continue;
        }
        if (section == "beginbfrange")
        {
            // <low> <high> <start>  or  <low> <high> [<dst> <dst> ...]
            if (token.kind == TokenKind::ArrayOpen && operands.size() == 2)
            {
                ToUnicodeMap::Range range;
                range.low = CodeValue(operands[0].text);
                range.high = CodeValue(operands[1].text);
                range.length = std::max<std::size_t>(1, std::min<std::size_t>(4, operands[0].text.size()));
                for (Token item = lexer.Next(); item.kind == TokenKind::String; item = lexer.Next())
                {
                    if (range.each.size() < 65536)
                        range.each.push_back(item.text);
                }
                if (range.low <= range.high && range.high - range.low < 65536)
                {
                    map.entries += range.each.size();
                    map.ranges.push_back(std::move(range));
                }
                operands.clear();
                continue;
            }
            if (token.kind != TokenKind::String)
                continue;
            operands.push_back(token);
            if (operands.size() == 3)
            {
                ToUnicodeMap::Range range;
                range.low = CodeValue(operands[0].text);
                range.high = CodeValue(operands[1].text);
                range.length = std::max<std::size_t>(1, std::min<std::size_t>(4, operands[0].text.size()));
                range.start = operands[2].text;
                if (range.low <= range.high && range.high - range.low < 65536)
                {
                    map.entries += 1;
                    map.ranges.push_back(std::move(range));
                }
                operands.clear();
            }
            continue;
        }
        if (!section.empty() && token.kind == TokenKind::String)
            operands.push_back(token);
    }
    return map;
}

struct FontInfo
{
    std::shared_ptr<ToUnicodeMap> toUnicode;
    bool composite = false;
};

class PdfDocument
{
public:
    explicit PdfDocument(const std::string_view bytes) : file(bytes) {}

    bool Load(std::string& error)
    {
        ScanObjects();
        if (objects.empty())
        {
            error = "No PDF objects were found; the file may be damaged.";
            return false;
        }
        if (encrypted)
        {
            error = "This PDF is encrypted (password protected or restricted), so its text cannot be read.";
            return false;
        }
        ExpandObjectStreams();
        return true;
    }

    std::vector<int> Pages()
    {
        std::vector<int> pages;
        for (const auto& [number, object] : objects)
        {
            const Value* type = object.value.Get("Type");
            if (type && type->IsName("Catalog"))
            {
                if (const Value* root = object.value.Get("Pages"); root && root->kind == Value::Kind::Reference)
                {
                    std::set<int> visited;
                    CollectPages(root->reference, pages, visited, 0, nullptr);
                }
                if (!pages.empty())
                    return pages;
            }
        }
        // No usable page tree: take every page object in number order.
        for (const auto& [number, object] : objects)
        {
            const Value* type = object.value.Get("Type");
            if (type && type->IsName("Page") && pages.size() < MaximumPages)
                pages.push_back(number);
        }
        return pages;
    }

    const Value* Resolve(const Value* value, int hops = 0) const
    {
        while (value && value->kind == Value::Kind::Reference && hops++ < 8)
        {
            const auto found = objects.find(value->reference);
            value = found == objects.end() ? nullptr : &found->second.value;
        }
        return value;
    }

    const PdfObject* Object(const int number) const
    {
        const auto found = objects.find(number);
        return found == objects.end() ? nullptr : &found->second;
    }

    std::optional<std::string> DecodeStream(const PdfObject& object)
    {
        if (!object.hasStream || decodedTotal > MaximumDecodedTotal)
            return std::nullopt;
        std::vector<std::string> filters;
        if (const Value* filter = Resolve(object.value.Get("Filter")))
        {
            if (filter->kind == Value::Kind::Name)
                filters.push_back(filter->text);
            else if (filter->kind == Value::Kind::Array)
                for (const Value& item : filter->items)
                    if (item.kind == Value::Kind::Name)
                        filters.push_back(item.text);
        }
        std::string data(object.stream.substr(0, std::min(object.stream.size(), MaximumStreamBytes)));
        for (const std::string& filter : filters)
        {
            if (filter == "FlateDecode" || filter == "Fl")
            {
                InflateResult inflated = InflateZlib(data, MaximumStreamBytes);
                // A damaged stream still yields the text decoded before the damage.
                if (inflated.output.empty() && inflated.status != InflateStatus::Complete)
                    inflated = InflateRaw(data, MaximumStreamBytes);
                if (inflated.output.empty() && inflated.status != InflateStatus::Complete)
                    return std::nullopt;
                data = std::move(inflated.output);
            }
            else if (filter == "ASCIIHexDecode" || filter == "AHx")
            {
                data = DecodeAsciiHex(data);
            }
            else if (filter == "ASCII85Decode" || filter == "A85")
            {
                data = DecodeAscii85(data);
            }
            else
            {
                return std::nullopt;
            }
        }
        decodedTotal += data.size();
        return data;
    }

    std::size_t ObjectCount() const { return objects.size(); }

private:
    static std::string DecodeAsciiHex(const std::string_view data)
    {
        std::string output;
        int high = -1;
        for (const char character : data)
        {
            if (character == '>')
                break;
            const int value = HexValue(character);
            if (value < 0)
                continue;
            if (high < 0)
                high = value;
            else
            {
                output.push_back(static_cast<char>(high * 16 + value));
                high = -1;
            }
        }
        if (high >= 0)
            output.push_back(static_cast<char>(high * 16));
        return output;
    }

    static std::string DecodeAscii85(const std::string_view data)
    {
        std::string output;
        std::uint64_t group = 0;
        int count = 0;
        for (std::size_t index = 0; index < data.size(); ++index)
        {
            const char character = data[index];
            if (character == '~')
                break;
            if (IsWhitespace(character))
                continue;
            if (character == 'z' && count == 0)
            {
                output.append(4, '\0');
                continue;
            }
            if (character < '!' || character > 'u')
                return output;
            group = group * 85 + static_cast<std::uint64_t>(character - '!');
            if (++count == 5)
            {
                for (int shift = 24; shift >= 0; shift -= 8)
                    output.push_back(static_cast<char>((group >> shift) & 0xFF));
                group = 0;
                count = 0;
            }
        }
        if (count > 1)
        {
            for (int pad = count; pad < 5; ++pad)
                group = group * 85 + 84;
            for (int byte = 0; byte < count - 1; ++byte)
                output.push_back(static_cast<char>((group >> (24 - 8 * byte)) & 0xFF));
        }
        return output;
    }

    void ScanObjects()
    {
        std::size_t cursor = 0;
        while (cursor < file.size() && objects.size() < MaximumObjects)
        {
            const std::size_t at = file.find("obj", cursor);
            if (at == std::string_view::npos)
                break;
            cursor = at + 3;
            if (at + 3 < file.size() && IsRegular(file[at + 3]))
                continue;
            // Walk back over "<number> <generation> ".
            std::size_t back = at;
            const auto skipSpace = [&]()
            {
                const std::size_t before = back;
                while (back > 0 && IsWhitespace(file[back - 1]))
                    --back;
                return back != before;
            };
            const auto readDigits = [&]()
            {
                const std::size_t end = back;
                while (back > 0 && std::isdigit(static_cast<unsigned char>(file[back - 1])) && end - back < 10)
                    --back;
                return end - back;
            };
            if (!skipSpace() || readDigits() == 0 || !skipSpace())
                continue;
            const std::size_t numberEnd = back;
            if (readDigits() == 0 || (back > 0 && IsRegular(file[back - 1])))
                continue;
            long long parsed = 0;
            for (const char digit : file.substr(back, numberEnd - back))
                parsed = std::min<long long>(parsed * 10 + (digit - '0'), 2147483647LL);
            const int number = static_cast<int>(parsed);

            Lexer lexer(file, at + 3);
            auto value = ParseValue(lexer);
            if (!value)
                continue;
            PdfObject object;
            object.value = std::move(*value);
            const std::size_t afterValue = lexer.Position();
            const Token next = lexer.Next();
            if (next.kind == TokenKind::Keyword && next.text == "stream")
            {
                std::size_t start = lexer.Position();
                if (start < file.size() && file[start] == '\r')
                    ++start;
                if (start < file.size() && file[start] == '\n')
                    ++start;
                std::size_t length = std::string_view::npos;
                if (const Value* declared = object.value.Get("Length"); declared && declared->kind == Value::Kind::Number &&
                                                                        declared->number >= 0 && declared->number <= file.size() - start)
                {
                    const auto candidate = static_cast<std::size_t>(declared->number);
                    std::size_t probe = start + candidate;
                    while (probe < file.size() && IsWhitespace(file[probe]))
                        ++probe;
                    if (file.substr(probe, 9) == "endstream")
                        length = candidate;
                }
                if (length == std::string_view::npos)
                {
                    const std::size_t end = file.find("endstream", start);
                    if (end == std::string_view::npos)
                        continue;
                    std::size_t trimmed = end;
                    if (trimmed > start && file[trimmed - 1] == '\n')
                        --trimmed;
                    if (trimmed > start && file[trimmed - 1] == '\r')
                        --trimmed;
                    length = trimmed - start;
                }
                object.hasStream = true;
                object.stream = file.substr(start, length);
                cursor = start + length;
            }
            else
            {
                cursor = std::max(cursor, afterValue);
            }
            if (const Value* type = object.value.Get("Type"); type && type->IsName("XRef") && object.value.Get("Encrypt"))
                encrypted = true;
            objects[number] = std::move(object);
        }
        // Classic trailers carry the encryption dictionary.
        for (std::size_t at = file.find("trailer"); at != std::string_view::npos; at = file.find("trailer", at + 7))
        {
            Lexer lexer(file, at + 7);
            if (const auto trailer = ParseValue(lexer); trailer && trailer->Get("Encrypt"))
                encrypted = true;
        }
    }

    void ExpandObjectStreams()
    {
        std::vector<int> containers;
        for (const auto& [number, object] : objects)
        {
            const Value* type = object.value.Get("Type");
            if (object.hasStream && type && type->IsName("ObjStm"))
                containers.push_back(number);
        }
        for (const int container : containers)
        {
            const PdfObject& source = objects.at(container);
            const Value* count = source.value.Get("N");
            const Value* first = source.value.Get("First");
            if (!count || !first || count->kind != Value::Kind::Number || first->kind != Value::Kind::Number)
                continue;
            auto decoded = DecodeStream(source);
            if (!decoded)
                continue;
            auto buffer = std::make_shared<std::string>(std::move(*decoded));
            Lexer header(*buffer);
            const auto total = static_cast<std::size_t>(std::clamp(count->number, 0.0, 100000.0));
            const auto base = static_cast<std::size_t>(std::max(0.0, first->number));
            std::vector<std::pair<int, std::size_t>> members;
            for (std::size_t index = 0; index < total; ++index)
            {
                const Token number = header.Next();
                const Token offset = header.Next();
                if (number.kind != TokenKind::Number || offset.kind != TokenKind::Number)
                    break;
                members.emplace_back(static_cast<int>(number.number), static_cast<std::size_t>(std::max(0.0, offset.number)));
            }
            for (const auto& [number, offset] : members)
            {
                if (objects.contains(number) || base + offset >= buffer->size() || objects.size() >= MaximumObjects)
                    continue;
                Lexer lexer(*buffer, base + offset);
                if (auto value = ParseValue(lexer))
                {
                    PdfObject object;
                    object.value = std::move(*value);
                    objects[number] = std::move(object);
                }
            }
            buffers.push_back(std::move(buffer));
        }
    }

    void CollectPages(const int number, std::vector<int>& pages, std::set<int>& visited, const int depth, const Value* inherited)
    {
        if (depth > 64 || pages.size() >= MaximumPages || !visited.insert(number).second)
            return;
        const PdfObject* node = Object(number);
        if (!node)
            return;
        const Value* type = node->value.Get("Type");
        const Value* resources = node->value.Get("Resources");
        if (!resources)
            resources = inherited;
        if (type && type->IsName("Page"))
        {
            pages.push_back(number);
            if (resources && !node->value.Get("Resources"))
                inheritedResources[number] = resources;
            return;
        }
        const Value* kids = Resolve(node->value.Get("Kids"));
        if (!kids || kids->kind != Value::Kind::Array)
            return;
        for (const Value& kid : kids->items)
            if (kid.kind == Value::Kind::Reference)
                CollectPages(kid.reference, pages, visited, depth + 1, resources);
    }

public:
    std::map<int, const Value*> inheritedResources;

private:
    std::string_view file;
    std::map<int, PdfObject> objects;
    std::vector<std::shared_ptr<std::string>> buffers;
    std::size_t decodedTotal = 0;
    bool encrypted = false;
};

// Turns content streams into text. Only the operators that place text are interpreted;
// graphics are skipped. Spacing is a heuristic from text positioning, good enough for
// search and quoting, not a layout engine.
class TextCollector
{
public:
    explicit TextCollector(PdfDocument& document) : pdf(document) {}

    void RunPage(const int pageNumber)
    {
        const PdfObject* page = pdf.Object(pageNumber);
        if (!page)
            return;
        const Value* resources = pdf.Resolve(page->value.Get("Resources"));
        if (!resources)
        {
            const auto inherited = pdf.inheritedResources.find(pageNumber);
            if (inherited != pdf.inheritedResources.end())
                resources = pdf.Resolve(inherited->second);
        }
        const Value* contents = pdf.Resolve(page->value.Get("Contents"));
        std::string stream;
        const auto appendStream = [&](const Value& reference)
        {
            if (reference.kind != Value::Kind::Reference)
                return;
            if (const PdfObject* object = pdf.Object(reference.reference))
                if (auto decoded = pdf.DecodeStream(*object))
                {
                    stream += *decoded;
                    stream += '\n';
                }
        };
        const Value* direct = page->value.Get("Contents");
        if (direct && direct->kind == Value::Kind::Reference)
        {
            const PdfObject* object = pdf.Object(direct->reference);
            if (object && object->hasStream)
                appendStream(*direct);
            else if (contents && contents->kind == Value::Kind::Array)
                for (const Value& item : contents->items)
                    appendStream(item);
        }
        else if (contents && contents->kind == Value::Kind::Array)
        {
            for (const Value& item : contents->items)
                appendStream(item);
        }
        Run(stream, resources, 0);
    }

    std::string text;
    std::size_t decodedGlyphs = 0;
    std::size_t undecodedGlyphs = 0;
    std::size_t limit = 0;

private:
    void Newline()
    {
        if (!text.empty() && text.back() != '\n')
            text.push_back('\n');
    }

    void Space()
    {
        if (!text.empty() && text.back() != ' ' && text.back() != '\n')
            text.push_back(' ');
    }

    const FontInfo* Font(const Value* resources, const std::string& name)
    {
        const Value* fonts = resources ? pdf.Resolve(resources->Get("Font")) : nullptr;
        const Value* entry = fonts ? fonts->Get(name) : nullptr;
        if (!entry)
            return nullptr;
        const int key = entry->kind == Value::Kind::Reference ? entry->reference : -1;
        if (key >= 0)
            if (const auto cached = fontCache.find(key); cached != fontCache.end())
                return &cached->second;
        const Value* font = pdf.Resolve(entry);
        if (!font || font->kind != Value::Kind::Dictionary)
            return nullptr;
        FontInfo info;
        const Value* subtype = font->Get("Subtype");
        info.composite = subtype && subtype->IsName("Type0");
        if (const Value* map = font->Get("ToUnicode"); map && map->kind == Value::Kind::Reference)
        {
            if (const auto cached = cmapCache.find(map->reference); cached != cmapCache.end())
            {
                info.toUnicode = cached->second;
            }
            else if (const PdfObject* object = pdf.Object(map->reference))
            {
                if (auto decoded = pdf.DecodeStream(*object))
                    info.toUnicode = std::make_shared<ToUnicodeMap>(ParseToUnicode(*decoded));
                cmapCache[map->reference] = info.toUnicode;
            }
        }
        if (key < 0)
        {
            anonymousFonts.push_back(std::move(info));
            return &anonymousFonts.back();
        }
        return &fontCache.emplace(key, std::move(info)).first->second;
    }

    void Show(const std::string& bytes)
    {
        if (text.size() > limit)
            return;
        if (currentFont && currentFont->toUnicode)
        {
            const ToUnicodeMap& map = *currentFont->toUnicode;
            for (std::size_t at = 0; at < bytes.size();)
            {
                const std::size_t length = map.CodeLength(std::string_view(bytes).substr(at), currentFont->composite ? 2 : 1);
                if (length == 0)
                    break;
                const std::uint32_t code = CodeValue(std::string_view(bytes).substr(at, length));
                if (const auto mapped = map.Lookup(code, length))
                {
                    text += *mapped;
                    ++decodedGlyphs;
                }
                else if (!currentFont->composite && length == 1)
                {
                    AppendWinAnsi(text, static_cast<unsigned char>(bytes[at]));
                    ++decodedGlyphs;
                }
                else
                {
                    ++undecodedGlyphs;
                }
                at += length;
            }
            return;
        }
        if (currentFont && currentFont->composite)
        {
            // Two-byte glyph identifiers with no map back to text cannot be read honestly.
            undecodedGlyphs += bytes.size() / 2;
            return;
        }
        for (const char byte : bytes)
            AppendWinAnsi(text, static_cast<unsigned char>(byte));
        decodedGlyphs += bytes.size();
    }

    void Run(const std::string_view stream, const Value* resources, const int depth)
    {
        if (depth > MaximumFormDepth)
            return;
        Lexer lexer(stream);
        std::vector<Value> operands;
        double lastY = 0.0;
        bool haveY = false;
        for (;;)
        {
            if (text.size() > limit)
                return;
            const std::size_t saved = lexer.Position();
            const Token token = lexer.Next();
            if (token.kind == TokenKind::End)
                return;
            if (token.kind != TokenKind::Keyword)
            {
                lexer.Seek(saved);
                auto operand = ParseValue(lexer);
                if (!operand)
                    continue;
                if (operands.size() < 64)
                    operands.push_back(std::move(*operand));
                continue;
            }
            const std::string& op = token.text;
            const auto number = [&](const std::size_t fromEnd) -> double
            {
                if (operands.size() < fromEnd)
                    return 0.0;
                const Value& value = operands[operands.size() - fromEnd];
                return value.kind == Value::Kind::Number ? value.number : 0.0;
            };
            if (op == "BI")
            {
                // An inline image: its binary data runs from ID to a whitespace-delimited EI.
                const std::string_view rest = stream.substr(lexer.Position());
                std::size_t data = rest.find("ID");
                std::size_t end = data == std::string_view::npos ? std::string_view::npos : rest.find("EI", data + 2);
                while (end != std::string_view::npos &&
                       !(IsWhitespace(rest[end - 1]) && (end + 2 >= rest.size() || IsWhitespace(rest[end + 2]))))
                    end = rest.find("EI", end + 2);
                if (end == std::string_view::npos)
                    return;
                lexer.Seek(lexer.Position() + end + 2);
            }
            else if (op == "BT")
            {
                haveY = false;
            }
            else if (op == "ET")
            {
                Space();
            }
            else if (op == "Tf" && operands.size() >= 2 && operands[operands.size() - 2].kind == Value::Kind::Name)
            {
                currentFont = Font(resources, operands[operands.size() - 2].text);
            }
            else if (op == "Td" || op == "TD")
            {
                if (number(1) != 0.0)
                    Newline();
                else if (number(2) > 0.0)
                    Space();
            }
            else if (op == "Tm")
            {
                const double y = number(1);
                if (haveY && std::abs(y - lastY) > 0.5)
                    Newline();
                else
                    Space();
                lastY = y;
                haveY = true;
            }
            else if (op == "T*")
            {
                Newline();
            }
            else if ((op == "Tj" || op == "'" || op == "\"") && !operands.empty() && operands.back().kind == Value::Kind::String)
            {
                if (op != "Tj")
                    Newline();
                Show(operands.back().text);
            }
            else if (op == "TJ" && !operands.empty() && operands.back().kind == Value::Kind::Array)
            {
                for (const Value& item : operands.back().items)
                {
                    if (item.kind == Value::Kind::String)
                        Show(item.text);
                    else if (item.kind == Value::Kind::Number && item.number < -200.0)
                        Space();
                }
            }
            else if (op == "Do" && !operands.empty() && operands.back().kind == Value::Kind::Name)
            {
                RunForm(operands.back().text, resources, depth);
            }
            operands.clear();
        }
    }

    void RunForm(const std::string& name, const Value* resources, const int depth)
    {
        const Value* xobjects = resources ? pdf.Resolve(resources->Get("XObject")) : nullptr;
        const Value* entry = xobjects ? xobjects->Get(name) : nullptr;
        if (!entry || entry->kind != Value::Kind::Reference || !formsVisited.insert(entry->reference).second)
            return;
        const PdfObject* form = pdf.Object(entry->reference);
        const Value* subtype = form ? form->value.Get("Subtype") : nullptr;
        if (!form || !subtype || !subtype->IsName("Form"))
            return;
        const auto decoded = pdf.DecodeStream(*form);
        if (!decoded)
            return;
        const Value* own = pdf.Resolve(form->value.Get("Resources"));
        const FontInfo* outerFont = currentFont;
        Run(*decoded, own ? own : resources, depth + 1);
        currentFont = outerFont;
        formsVisited.erase(entry->reference);
    }

    PdfDocument& pdf;
    const FontInfo* currentFont = nullptr;
    std::map<int, FontInfo> fontCache;
    std::vector<FontInfo> anonymousFonts;
    std::map<int, std::shared_ptr<ToUnicodeMap>> cmapCache;
    std::set<int> formsVisited;
};

// Collapses the spacing the collector produced: no runs of spaces, no trailing spaces,
// at most one blank line.
std::string Tidy(const std::string& text)
{
    std::string output;
    output.reserve(text.size());
    int newlines = 0;
    for (const char character : text)
    {
        if (character == '\n')
        {
            while (!output.empty() && output.back() == ' ')
                output.pop_back();
            if (++newlines <= 2 && !output.empty())
                output.push_back('\n');
            continue;
        }
        if (character == ' ' || character == '\t')
        {
            if (!output.empty() && output.back() != ' ' && output.back() != '\n')
                output.push_back(' ');
            continue;
        }
        newlines = 0;
        output.push_back(character);
    }
    while (!output.empty() && (output.back() == ' ' || output.back() == '\n'))
        output.pop_back();
    return output;
}

std::size_t VisibleCharacters(const std::string& text)
{
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(),
        [](const char character) { return !IsWhitespace(character) && character != ' '; }));
}
} // namespace

ExtractedText ExtractPdfText(const std::string_view bytes, const std::size_t maxTextBytes)
{
    ExtractedText result;
    const std::size_t header = bytes.substr(0, 1024).find("%PDF-");
    if (header == std::string_view::npos)
    {
        result.reason = "This is not a PDF file.";
        return result;
    }
    PdfDocument document(bytes);
    std::string error;
    if (!document.Load(error))
    {
        result.reason = error;
        return result;
    }
    const std::vector<int> pages = document.Pages();
    if (pages.empty())
    {
        result.reason = "No pages were found in this PDF; it may be damaged.";
        return result;
    }

    TextCollector collector(document);
    collector.limit = maxTextBytes + 4096;
    std::string combined;
    for (const int page : pages)
    {
        collector.text.clear();
        collector.RunPage(page);
        std::string pageText = Tidy(collector.text);
        if (!combined.empty())
            combined += "\n\n";
        result.pageOffsets.push_back(combined.size());
        combined += pageText;
        if (combined.size() > maxTextBytes)
        {
            result.truncated = true;
            break;
        }
    }
    if (VisibleCharacters(combined) < 16)
    {
        result.pageOffsets.clear();
        result.reason = collector.undecodedGlyphs > 0
                            ? "This PDF's fonts carry no text mapping, so its words cannot be read reliably. Text recognition (OCR) is "
                              "not supported."
                            : "This PDF has no text layer (it may be scanned pages or images). Text recognition (OCR) is not supported.";
        return result;
    }
    if (combined.size() > maxTextBytes)
    {
        combined = revia::utf8::Prefix(combined, maxTextBytes);
        result.truncated = true;
    }
    while (!result.pageOffsets.empty() && result.pageOffsets.back() > combined.size())
        result.pageOffsets.pop_back();
    result.ok = revia::utf8::IsValid(combined);
    if (!result.ok)
    {
        result.reason = "The PDF's text could not be decoded as valid text.";
        return result;
    }
    result.text = std::move(combined);
    if (result.truncated)
        result.reason = "Only the first " + std::to_string(maxTextBytes / 1024) + " KB of text was kept.";
    else if (collector.undecodedGlyphs > collector.decodedGlyphs / 10)
        result.reason = "Some text in this PDF uses fonts without a text mapping and was left out.";
    return result;
}

} // namespace revia::attachments
