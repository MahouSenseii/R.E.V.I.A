#include "Evaluation/calculationVerifier.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace revia::evaluation
{
namespace
{
enum class Dimension
{
    Scalar,
    Time,
    Length,
    Mass
};

struct Quantity
{
    double value = 0;
    Dimension dimension = Dimension::Scalar;
};

struct Unit
{
    double scale = 1;
    Dimension dimension = Dimension::Scalar;
};

Unit ReadUnit(const std::string_view name)
{
    if (name.empty())
        return {};
    if (name == "s" || name == "sec" || name == "seconds")
        return {1, Dimension::Time};
    if (name == "min" || name == "minutes")
        return {60, Dimension::Time};
    if (name == "h" || name == "hours")
        return {3600, Dimension::Time};
    if (name == "mm")
        return {0.001, Dimension::Length};
    if (name == "cm")
        return {0.01, Dimension::Length};
    if (name == "m")
        return {1, Dimension::Length};
    if (name == "km")
        return {1000, Dimension::Length};
    if (name == "g")
        return {0.001, Dimension::Mass};
    if (name == "kg")
        return {1, Dimension::Mass};
    throw std::runtime_error("Unsupported unit.");
}

std::string BaseUnit(const Dimension dimension)
{
    if (dimension == Dimension::Time)
        return "s";
    if (dimension == Dimension::Length)
        return "m";
    if (dimension == Dimension::Mass)
        return "kg";
    return {};
}

void RequireFinite(const double value)
{
    if (!std::isfinite(value))
        throw std::runtime_error("Arithmetic overflow or nonfinite value.");
}

class ExpressionParser
{
  public:
    ExpressionParser(const std::string& expression, const std::stop_token token) : text(expression), stopToken(token)
    {
    }

    Quantity Parse()
    {
        const auto value = Additive();
        SkipSpace();
        if (position != text.size())
            throw std::runtime_error("Unsupported arithmetic syntax.");
        return value;
    }

    bool HasUnits() const
    {
        return hasUnits;
    }

  private:
    void SkipSpace()
    {
        if (stopToken.stop_requested())
            throw std::runtime_error("Calculation cancelled.");
        while (
            position < text.size() && (text[position] == ' ' || text[position] == '\t' || text[position] == '\r' || text[position] == '\n'))
            ++position;
    }

    bool Take(const char token)
    {
        SkipSpace();
        if (position == text.size() || text[position] != token)
            return false;
        ++position;
        return true;
    }

    void CountOperation()
    {
        if (++operations > 128)
            throw std::runtime_error("Arithmetic operation budget exceeded.");
    }

    Quantity Additive()
    {
        auto left = Multiplicative();
        for (;;)
        {
            const bool add = Take('+');
            if (!add && !Take('-'))
                return left;
            CountOperation();
            const auto right = Multiplicative();
            if (left.dimension != right.dimension)
                throw std::runtime_error("Addition requires matching dimensions.");
            left.value = add ? left.value + right.value : left.value - right.value;
            RequireFinite(left.value);
        }
    }

    Quantity Multiplicative()
    {
        auto left = Unary();
        for (;;)
        {
            const bool multiply = Take('*');
            if (!multiply && !Take('/'))
                return left;
            CountOperation();
            const auto right = Unary();
            if (multiply)
            {
                if (left.dimension != Dimension::Scalar && right.dimension != Dimension::Scalar)
                    throw std::runtime_error("Composite dimensions are unsupported.");
                left.value *= right.value;
                if (left.dimension == Dimension::Scalar)
                    left.dimension = right.dimension;
            }
            else
            {
                if (right.value == 0)
                    throw std::runtime_error("Division by zero.");
                if (right.dimension != Dimension::Scalar && left.dimension != right.dimension)
                    throw std::runtime_error("Composite dimensions are unsupported.");
                left.value /= right.value;
                if (right.dimension != Dimension::Scalar)
                    left.dimension = Dimension::Scalar;
            }
            RequireFinite(left.value);
        }
    }

    Quantity Unary()
    {
        bool negative = false;
        for (;;)
        {
            if (Take('+'))
                CountOperation();
            else if (Take('-'))
            {
                CountOperation();
                negative = !negative;
            }
            else
                break;
        }
        auto value = Primary();
        if (negative)
            value.value = -value.value;
        return value;
    }

    Quantity Primary()
    {
        if (Take('('))
        {
            if (++depth > 32)
                throw std::runtime_error("Arithmetic nesting budget exceeded.");
            const auto value = Additive();
            if (!Take(')'))
                throw std::runtime_error("Missing closing parenthesis.");
            --depth;
            return value;
        }
        SkipSpace();
        if (++literals > 64)
            throw std::runtime_error("Arithmetic literal budget exceeded.");
        const auto begin = position;
        bool digit = false;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9')
        {
            digit = true;
            ++position;
        }
        if (position < text.size() && text[position] == '.')
        {
            ++position;
            while (position < text.size() && text[position] >= '0' && text[position] <= '9')
            {
                digit = true;
                ++position;
            }
        }
        if (!digit)
            throw std::runtime_error("Expected finite decimal literal.");
        if (position < text.size() && (text[position] == 'e' || text[position] == 'E'))
        {
            ++position;
            if (position < text.size() && (text[position] == '+' || text[position] == '-'))
                ++position;
            const auto exponentStart = position;
            while (position < text.size() && text[position] >= '0' && text[position] <= '9')
                ++position;
            if (position == exponentStart)
                throw std::runtime_error("Invalid decimal exponent.");
        }
        double magnitude = 0;
        const auto conversion = std::from_chars(text.data() + begin, text.data() + position, magnitude);
        if (conversion.ec != std::errc{} || conversion.ptr != text.data() + position)
            throw std::runtime_error("Decimal literal is outside the supported range.");
        SkipSpace();
        const auto unitStart = position;
        while (position < text.size() && text[position] >= 'a' && text[position] <= 'z')
            ++position;
        const auto unit = ReadUnit(std::string_view(text).substr(unitStart, position - unitStart));
        hasUnits = hasUnits || unit.dimension != Dimension::Scalar;
        magnitude *= unit.scale;
        RequireFinite(magnitude);
        return {magnitude, unit.dimension};
    }

    const std::string& text;
    std::stop_token stopToken;
    std::size_t position = 0;
    int depth = 0;
    int operations = 0;
    int literals = 0;
    bool hasUnits = false;
};

std::string DecimalString(double value)
{
    if (value == 0)
        value = 0;
    char buffer[128];
    const auto formatted =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (formatted.ec != std::errc{})
        throw std::runtime_error("Unable to format calculation.");
    return std::string(buffer, formatted.ptr);
}
}

CalculationResult VerifyCalculation(const std::string& typedInputJson, const std::stop_token stopToken)
{
    CalculationResult result;
    result.limitations = "Verifies only the declared arithmetic using finite double precision; it does not verify "
                         "word-problem mapping, premises, measurements, or real-world correctness. Composite units are unsupported.";
    try
    {
        if (stopToken.stop_requested())
            throw std::runtime_error("Calculation cancelled.");
        if (typedInputJson.size() > 8192)
            throw std::runtime_error("Typed calculation input exceeds its budget.");
        std::set<std::string> keys;
        const auto input = nlohmann::json::parse(typedInputJson,
            [&keys](const int depth, const nlohmann::json::parse_event_t event, nlohmann::json& parsed)
            {
                if (depth > 8)
                    throw std::runtime_error("Typed input nesting exceeds its budget.");
                if (event == nlohmann::json::parse_event_t::key && !keys.insert(parsed.get<std::string>()).second)
                    throw std::runtime_error("Duplicate input field.");
                return true;
            });
        if (!input.is_object() || !input.contains("expression") || !input["expression"].is_string())
            throw std::runtime_error("Typed input requires a string expression.");
        for (const auto& item : input.items())
        {
            if (item.key() != "expression" && item.key() != "unit")
                throw std::runtime_error("Unknown input field.");
        }
        if (input.contains("unit") && !input["unit"].is_string())
            throw std::runtime_error("Unit must be a string.");
        result.expression = input["expression"].get<std::string>();
        if (result.expression.empty() || result.expression.size() > 2048)
            throw std::runtime_error("Expression is empty or exceeds its budget.");
        result.unit = input.value("unit", std::string{});
        const auto outputUnit = ReadUnit(result.unit);
        ExpressionParser parser(result.expression, stopToken);
        const auto calculated = parser.Parse();
        double value = calculated.value;
        if (calculated.dimension != Dimension::Scalar)
        {
            if (result.unit.empty())
                result.unit = BaseUnit(calculated.dimension);
            else if (outputUnit.dimension != calculated.dimension)
                throw std::runtime_error("Output unit dimension mismatch.");
            else
                value /= outputUnit.scale;
        }
        else if (parser.HasUnits() && !result.unit.empty())
        {
            throw std::runtime_error("Dimensionless result cannot acquire an output unit.");
        }
        RequireFinite(value);
        if (stopToken.stop_requested())
            throw std::runtime_error("Calculation cancelled.");
        result.value = DecimalString(value);
        result.succeeded = true;
    }
    catch (const std::exception& error)
    {
        result.refusal = error.what();
        result.value.clear();
    }
    return result;
}
}
