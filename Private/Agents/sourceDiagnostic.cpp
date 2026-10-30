#include "Agents/sourceDiagnostic.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <string_view>
#include <utility>

namespace revia::agents
{
namespace
{
enum class TokenKind
{
    Word,
    Number,
    Literal,
    Symbol
};

struct Token
{
    TokenKind kind;
    std::string_view text;
    std::size_t begin;
    std::size_t end;
};

bool WordByte(const char value)
{
    const auto byte = static_cast<unsigned char>(value);
    return std::isalnum(byte) != 0 || value == '_';
}

bool Space(const char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
}

bool Keyword(const std::string_view word)
{
    constexpr std::string_view words[] = {"alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case",
        "catch", "char", "char8_t", "char16_t", "char32_t", "class", "concept", "const", "consteval", "constexpr", "constinit",
        "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
        "else", "enum", "compl", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
        "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
        "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast",
        "struct", "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"};
    return std::find(std::begin(words), std::end(words), word) != std::end(words);
}

bool Name(const Token& token)
{
    return token.kind == TokenKind::Word && token.text != "std" && !Keyword(token.text);
}

bool HasEscapedLineJoin(const std::string_view source)
{
    for (std::size_t index = 0; index < source.size(); ++index)
    {
        if (source[index] != '\\')
            continue;
        auto next = index + 1;
        while (next < source.size() && (source[next] == ' ' || source[next] == '\t' || source[next] == '\v' || source[next] == '\f'))
            ++next;
        if (next < source.size() && (source[next] == '\r' || source[next] == '\n'))
            return true;
    }
    return false;
}

int HexDigit(const char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

std::optional<std::size_t> LiteralEnd(const std::string_view source, const std::size_t begin)
{
    for (const std::string_view prefix : {"u8R\"", "uR\"", "UR\"", "LR\"", "R\""})
    {
        if (!source.substr(begin).starts_with(prefix))
            continue;
        if (prefix != "R\"")
            return std::nullopt;
        const auto delimiter = begin + prefix.size();
        const auto opening = source.find('(', delimiter);
        if (opening == std::string_view::npos || opening - delimiter > 16)
            return std::nullopt;
        for (auto index = delimiter; index < opening; ++index)
            if (static_cast<unsigned char>(source[index]) <= 0x20 || static_cast<unsigned char>(source[index]) > 0x7e ||
                source[index] == ')' || source[index] == '\\')
                return std::nullopt;
        const auto close = ')' + std::string(source.substr(delimiter, opening - delimiter)) + '"';
        const auto end = source.find(close, opening + 1);
        return end == std::string_view::npos ? std::nullopt : std::optional{end + close.size()};
    }
    const std::size_t quote = begin;
    for (const std::string_view prefix : {"u8", "u", "U", "L"})
        if (source.substr(begin).starts_with(prefix) && begin + prefix.size() < source.size() &&
            (source[begin + prefix.size()] == '"' || source[begin + prefix.size()] == '\''))
        {
            return std::nullopt;
        }
    if (source[quote] != '"' && source[quote] != '\'')
        return begin;
    std::size_t characters = 0;
    for (auto index = quote + 1; index < source.size(); ++index)
    {
        if (source[index] == source[quote])
            return source[quote] == '\'' && characters != 1 ? std::nullopt : std::optional{index + 1};
        if (static_cast<unsigned char>(source[index]) < 0x20 || static_cast<unsigned char>(source[index]) > 0x7e)
            return std::nullopt;
        ++characters;
        if (source[index] == '\\')
        {
            if (++index == source.size())
                return std::nullopt;
            if (std::string_view("abfnrtv\\\'\"?").find(source[index]) != std::string_view::npos)
                continue;
            unsigned value = 0;
            if (source[index] >= '0' && source[index] <= '7')
            {
                value = static_cast<unsigned>(source[index] - '0');
                for (unsigned digits = 1; digits < 3 && index + 1 < source.size() && source[index + 1] >= '0' && source[index + 1] <= '7';
                    ++digits)
                    value = value * 8 + static_cast<unsigned>(source[++index] - '0');
                if (value > 127)
                    return std::nullopt;
                continue;
            }
            if (source[index] != 'x' || index + 1 == source.size() || HexDigit(source[index + 1]) < 0)
                return std::nullopt;
            while (index + 1 < source.size() && HexDigit(source[index + 1]) >= 0)
            {
                value = value * 16 + static_cast<unsigned>(HexDigit(source[++index]));
                if (value > 127)
                    return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

bool IncludeLine(std::string_view line)
{
    const auto consumeSpaces = [&line]()
    {
        while (!line.empty() && Space(line.front()))
            line.remove_prefix(1);
    };
    consumeSpaces();
    if (!line.starts_with("include"))
        return false;
    line.remove_prefix(7);
    consumeSpaces();
    for (const std::string_view header : {"<vector>", "<string>", "<cstddef>", "<cstdint>", "<stdexcept>"})
        if (line.starts_with(header))
        {
            line.remove_prefix(header.size());
            consumeSpaces();
            return line.empty() || line.starts_with("//");
        }
    return false;
}

bool Tokenize(const std::string_view source, std::vector<Token>& tokens)
{
    if (HasEscapedLineJoin(source))
        return false;
    bool lineStart = true;
    for (std::size_t index = 0; index < source.size();)
    {
        const auto begin = index;
        if (Space(source[index]))
        {
            lineStart = lineStart || source[index] == '\n' || source[index] == '\r';
            ++index;
            continue;
        }
        if (source.substr(index).starts_with("//"))
        {
            while (index < source.size() && source[index] != '\r' && source[index] != '\n')
                ++index;
            continue;
        }
        if (source.substr(index).starts_with("/*"))
        {
            const auto end = source.find("*/", index + 2);
            if (end == std::string_view::npos)
                return false;
            lineStart = lineStart || source.substr(index, end + 2 - index).find_first_of("\r\n") != std::string_view::npos;
            index = end + 2;
            continue;
        }
        if (source[index] == '#')
        {
            const auto end = source.find_first_of("\r\n", index);
            const auto line = source.substr(index + 1, end == std::string_view::npos ? source.size() - index - 1 : end - index - 1);
            if (!lineStart || !IncludeLine(line) || line.find('\\') != std::string_view::npos)
                return false;
            index = end == std::string_view::npos ? source.size() : end;
            continue;
        }
        if (source.substr(index).starts_with("%:"))
            return false;
        const auto literal = LiteralEnd(source, index);
        if (!literal)
            return false;
        if (*literal > index)
        {
            index = *literal;
            tokens.push_back({TokenKind::Literal, source.substr(begin, index - begin), begin, index});
        }
        else if (std::isalpha(static_cast<unsigned char>(source[index])) || source[index] == '_')
        {
            while (index < source.size() && WordByte(source[index]))
                ++index;
            if (index - begin > 128)
                return false;
            tokens.push_back({TokenKind::Word, source.substr(begin, index - begin), begin, index});
        }
        else if (std::isdigit(static_cast<unsigned char>(source[index])))
        {
            ++index;
            while (index < source.size() && (WordByte(source[index]) || source[index] == '.' || source[index] == '\''))
                ++index;
            tokens.push_back({TokenKind::Number, source.substr(begin, index - begin), begin, index});
        }
        else
        {
            if (static_cast<unsigned char>(source[index]) < 0x20 || static_cast<unsigned char>(source[index]) >= 0x80 ||
                source[index] == '\\')
                return false;
            ++index;
            for (const std::string_view symbol : {"::", "<=", ">=", "==", "!=", "++", "--", "&&", "||", "+=", "-=", "->"})
                if (source.substr(begin).starts_with(symbol))
                {
                    index = begin + symbol.size();
                    break;
                }
            tokens.push_back({TokenKind::Symbol, source.substr(begin, index - begin), begin, index});
        }
        lineStart = false;
        if (tokens.size() > 8192)
            return false;
    }
    return true;
}

bool Boundaries(const std::vector<Token>& tokens, std::vector<std::size_t>& matches)
{
    std::vector<std::size_t> stack;
    matches.resize(tokens.size(), tokens.size());
    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
        if (tokens[index].kind != TokenKind::Symbol)
            continue;
        const auto text = tokens[index].text;
        if (text == "(" || text == "[" || text == "{")
            stack.push_back(index);
        else if (text == ")" || text == "]" || text == "}")
        {
            if (stack.empty())
                return false;
            const auto opening = tokens[stack.back()].text;
            if ((opening == "(" && text != ")") || (opening == "[" && text != "]") || (opening == "{" && text != "}"))
                return false;
            matches[index] = stack.back();
            matches[stack.back()] = index;
            stack.pop_back();
        }
    }
    return stack.empty();
}

struct Cursor
{
    const std::vector<Token>& tokens;
    std::size_t position;
    std::size_t end;

    bool Take(const std::string_view expected)
    {
        if (position == end || tokens[position].text != expected)
            return false;
        ++position;
        return true;
    }

    bool TakeName(std::string& name)
    {
        if (position == end || !Name(tokens[position]))
            return false;
        name = tokens[position++].text;
        return true;
    }

    bool Done() const
    {
        return position == end;
    }
};

bool BuiltinWord(const std::string_view word)
{
    constexpr std::string_view words[] = {
        "bool", "char", "char8_t", "char16_t", "char32_t", "wchar_t", "short", "int", "long", "signed", "unsigned", "float", "double"};
    return std::find(std::begin(words), std::end(words), word) != std::end(words);
}

std::string Builtin(Cursor& cursor)
{
    std::string spelling;
    while (cursor.position < cursor.end && BuiltinWord(cursor.tokens[cursor.position].text))
    {
        if (!spelling.empty())
            spelling += ' ';
        spelling += cursor.tokens[cursor.position++].text;
    }
    for (const std::string_view canonical :
        {"bool", "char", "char8_t", "char16_t", "char32_t", "wchar_t", "float", "double", "long double", "unsigned char", "signed char",
            "unsigned short", "short", "unsigned int", "int", "unsigned long", "long", "unsigned long long", "long long"})
    {
        const bool integerSuffix = canonical == "short" || canonical == "unsigned short" || canonical == "long" ||
                                   canonical == "unsigned long" || canonical == "long long" || canonical == "unsigned long long";
        if (spelling == canonical || (integerSuffix && spelling == std::string(canonical) + " int"))
            return std::string(canonical);
    }
    if (spelling == "signed" || spelling == "signed int")
        return "int";
    if (spelling == "unsigned")
        return "unsigned int";
    return {};
}

struct Type
{
    std::string value;
    bool constant = false;
    bool reference = false;
};

bool SimpleType(Cursor& cursor, Type& type, const bool references = true)
{
    type.constant = cursor.Take("const");
    if (cursor.position < cursor.end && cursor.tokens[cursor.position].text == "std")
    {
        if (!cursor.Take("std") || !cursor.Take("::") || !cursor.Take("string"))
            return false;
        type.value = "std::string";
    }
    else
        type.value = Builtin(cursor);
    if (type.value.empty())
        return false;
    if (cursor.Take("const"))
    {
        if (type.constant)
            return false;
        type.constant = true;
    }
    type.reference = references && cursor.Take("&");
    return true;
}

struct Parameter
{
    Type type;
    std::string name;
    bool vector = false;
};

bool ParseParameter(Cursor cursor, Parameter& parameter)
{
    auto vector = cursor;
    const bool leadingConst = vector.Take("const");
    if (vector.Take("std") && vector.Take("::") && vector.Take("vector") && vector.Take("<"))
    {
        parameter.type.value = Builtin(vector);
        if (parameter.type.value.empty() || !vector.Take(">"))
            return false;
        const bool trailingConst = vector.Take("const");
        parameter.type.constant = leadingConst || trailingConst;
        if ((leadingConst && trailingConst) || !vector.Take("&") || !vector.TakeName(parameter.name) || !vector.Done())
            return false;
        parameter.type.reference = true;
        parameter.vector = true;
        return true;
    }
    return SimpleType(cursor, parameter.type) && cursor.TakeName(parameter.name) && cursor.Done();
}

bool Parameters(const std::vector<Token>& tokens, const std::size_t begin, const std::size_t end, std::vector<Parameter>& parameters)
{
    if (begin == end || (end == begin + 1 && tokens[begin].text == "void"))
        return true;
    auto start = begin;
    for (auto index = begin; index <= end; ++index)
        if (index == end || tokens[index].text == ",")
        {
            Parameter parameter;
            if (!ParseParameter({tokens, start, index}, parameter) ||
                std::any_of(parameters.begin(), parameters.end(), [&](const auto& other) { return other.name == parameter.name; }))
                return false;
            parameters.push_back(std::move(parameter));
            start = index + 1;
        }
    return true;
}

std::string Span(const std::string_view source, const std::vector<Token>& tokens, const std::size_t begin, const std::size_t end)
{
    return std::string(source.substr(tokens[begin].begin, tokens[end - 1].end - tokens[begin].begin));
}

bool ReturnTrue(Cursor& cursor)
{
    const bool block = cursor.Take("{");
    return cursor.Take("return") && cursor.Take("true") && cursor.Take(";") && (!block || cursor.Take("}"));
}

std::optional<SourceDiagnostic> SearchBoundary(const std::string_view source, const std::vector<Token>& tokens, const std::string& function,
    const std::vector<Parameter>& parameters, const std::size_t begin, const std::size_t end)
{
    if (parameters.size() != 2)
        return std::nullopt;
    const auto vector = std::find_if(parameters.begin(), parameters.end(), [](const auto& parameter) { return parameter.vector; });
    if (vector == parameters.end() || !vector->type.constant)
        return std::nullopt;
    const auto& target = parameters[vector == parameters.begin() ? 1 : 0];
    if (target.vector || target.type.reference || target.type.value != vector->type.value)
        return std::nullopt;
    Cursor cursor{tokens, begin, end};
    std::string index;
    if (!cursor.Take("for") || !cursor.Take("(") || !cursor.Take("std") || !cursor.Take("::") || !cursor.Take("size_t") ||
        !cursor.TakeName(index) || index == vector->name || index == target.name || !cursor.Take("=") || !cursor.Take("0") ||
        !cursor.Take(";"))
        return std::nullopt;
    const auto conditionBegin = cursor.position;
    if (!cursor.Take(index))
        return std::nullopt;
    const auto comparison = cursor.position;
    if (!cursor.Take("<=") || !cursor.Take(vector->name) || !cursor.Take(".") || !cursor.Take("size") || !cursor.Take("(") ||
        !cursor.Take(")"))
        return std::nullopt;
    const auto conditionEnd = cursor.position;
    if (!cursor.Take(";"))
        return std::nullopt;
    if (!(cursor.Take("++") ? cursor.Take(index) : cursor.Take(index) && cursor.Take("++")) || !cursor.Take(")"))
        return std::nullopt;
    const bool loopBlock = cursor.Take("{");
    if (!cursor.Take("if") || !cursor.Take("(") || !cursor.Take(vector->name))
        return std::nullopt;
    const bool checked = cursor.Take(".");
    if (checked ? !cursor.Take("at") || !cursor.Take("(") || !cursor.Take(index) || !cursor.Take(")")
                : !cursor.Take("[") || !cursor.Take(index) || !cursor.Take("]"))
        return std::nullopt;
    if (!cursor.Take("==") || !cursor.Take(target.name) || !cursor.Take(")") || !ReturnTrue(cursor) || (loopBlock && !cursor.Take("}")) ||
        !cursor.Take("return") || !cursor.Take("false") || !cursor.Take(";") || !cursor.Done())
        return std::nullopt;
    SourceDiagnostic result;
    result.kind = checked ? SourceDiagnosticKind::CheckedRangeBoundary : SourceDiagnosticKind::UncheckedRangeBoundary;
    result.function = function;
    result.evidence = result.original = Span(source, tokens, conditionBegin, conditionEnd);
    result.replacement = result.original;
    result.replacement.replace(tokens[comparison].begin - tokens[conditionBegin].begin, 2, "<");
    result.condition = "When `" + vector->name + "` is empty or `" + target.name + "` is absent, `" + index + "` reaches `" + vector->name +
                       ".size()`; a matching target returns earlier.";
    result.consequence = checked ? "`" + vector->name + ".at(" + index + ")` throws `std::out_of_range` at that boundary."
                                 : "`" + vector->name + "[" + index +
                                       "]` violates the vector index precondition; the unchecked access has undefined behavior.";
    result.correction = "Change `" + index + " <= " + vector->name + ".size()` to `" + index + " < " + vector->name + ".size()`.";
    return result;
}

bool StringInitializer(Cursor& cursor)
{
    const auto start = cursor.position;
    while (cursor.position < cursor.end && cursor.tokens[cursor.position].kind == TokenKind::Literal &&
           (cursor.tokens[cursor.position].text.starts_with('"') || cursor.tokens[cursor.position].text.starts_with("R\"")))
        ++cursor.position;
    return cursor.position > start;
}

bool ScalarInitializer(Cursor& cursor, const Type& type)
{
    if (cursor.Take("true") || cursor.Take("false"))
        return true;
    (void)cursor.Take("+");
    if (cursor.position == cursor.end || cursor.tokens[cursor.position].kind != TokenKind::Number)
        return false;
    const auto text = cursor.tokens[cursor.position++].text;
    if (text.size() > 1 && text.front() == '0')
        return false;
    unsigned value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value <= (type.value == "bool" ? 1u : 127u);
}

bool LocalInitializer(Cursor& cursor, const Type& type)
{
    const bool assignment = cursor.Take("=");
    const bool braces = cursor.Take("{");
    const bool parentheses = !assignment && !braces && cursor.Take("(");
    if (!assignment && !braces && !parentheses && cursor.position < cursor.end && cursor.tokens[cursor.position].text == ";")
        return type.value == "std::string";
    if (!assignment && !braces && !parentheses)
        return false;
    if (braces && cursor.Take("}"))
        return true;
    if (!(type.value == "std::string" ? StringInitializer(cursor) : ScalarInitializer(cursor, type)))
        return false;
    return (!braces || cursor.Take("}")) && (!parentheses || cursor.Take(")"));
}

std::optional<SourceDiagnostic> LocalReference(const std::string_view source, const std::vector<Token>& tokens, const std::string& function,
    const Type& returned, const std::vector<Parameter>& parameters, const std::size_t typeBegin, const std::size_t typeEnd,
    const std::size_t begin, const std::size_t end)
{
    if (!returned.reference)
        return std::nullopt;
    Cursor cursor{tokens, begin, end};
    Type local;
    std::string name;
    if (!SimpleType(cursor, local, false) || local.value != returned.value || (local.constant && !returned.constant) ||
        !cursor.TakeName(name) ||
        std::any_of(parameters.begin(), parameters.end(), [&](const auto& parameter) { return parameter.name == name; }) ||
        !LocalInitializer(cursor, local) || !cursor.Take(";"))
        return std::nullopt;
    const auto returnBegin = cursor.position;
    if (!cursor.Take("return"))
        return std::nullopt;
    const bool parentheses = cursor.Take("(");
    if (!cursor.Take(name) || (parentheses && !cursor.Take(")")) || !cursor.Take(";") || !cursor.Done())
        return std::nullopt;
    SourceDiagnostic result;
    result.kind = SourceDiagnosticKind::LocalReferenceReturn;
    result.function = function;
    result.evidence = Span(source, tokens, returnBegin, cursor.position);
    result.original = Span(source, tokens, typeBegin, typeEnd);
    result.replacement = returned.value;
    result.condition =
        "When the caller uses the returned reference after the function returns, the local `" + name + "` is no longer alive.";
    result.consequence = "The returned reference dangles; accessing the destroyed object through it has undefined behavior.";
    result.correction = "Change return type `" + std::string(returned.constant ? "const " : "") + returned.value + "&` to `" +
                        returned.value + "` so the result is returned by value.";
    return result;
}
}

std::vector<SourceDiagnostic> AnalyzeSourceDiagnostics(const std::string_view source)
{
    if (source.size() > 65536)
        return {};
    std::vector<Token> tokens;
    std::vector<std::size_t> matches;
    if (!Tokenize(source, tokens) || !Boundaries(tokens, matches))
        return {};
    for (const auto& token : tokens)
        if (token.kind == TokenKind::Word && (token.text == "using" || token.text == "typedef" || token.text == "namespace" ||
                                                 token.text == "class" || token.text == "struct" || token.text == "union" ||
                                                 token.text == "enum" || token.text == "template" || token.text == "operator"))
            return {};
    std::vector<SourceDiagnostic> diagnostics;
    for (std::size_t begin = 0; begin < tokens.size();)
    {
        if (tokens[begin].text == ";")
        {
            ++begin;
            continue;
        }
        auto body = begin;
        while (body < tokens.size() && tokens[body].text != "{" && tokens[body].text != ";")
            if (tokens[body].text == "(" || tokens[body].text == "[")
                body = matches[body] + 1;
            else
                ++body;
        if (body == tokens.size() || tokens[body].text != "{" || body == begin || tokens[body - 1].text != ")")
            return {};
        const auto arguments = matches[body - 1];
        if (arguments <= begin || !Name(tokens[arguments - 1]))
            return {};
        const auto typeEnd = arguments - 1;
        Cursor typeCursor{tokens, begin, typeEnd};
        Type returned;
        std::vector<Parameter> parameters;
        const auto end = matches[body];
        if (SimpleType(typeCursor, returned) && typeCursor.Done() && Parameters(tokens, arguments + 1, body - 1, parameters))
        {
            const std::string function(tokens[typeEnd].text);
            if (returned.value == "bool" && !returned.reference)
                if (auto range = SearchBoundary(source, tokens, function, parameters, body + 1, end))
                    diagnostics.push_back(std::move(*range));
            if (auto local = LocalReference(source, tokens, function, returned, parameters, begin, typeEnd, body + 1, end))
                diagnostics.push_back(std::move(*local));
        }
        begin = end + 1;
    }
    return diagnostics;
}
}
