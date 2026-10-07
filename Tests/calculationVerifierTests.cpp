#include "Evaluation/calculationVerifier.h"
#include "testSupport.h"

#include <nlohmann/json.hpp>

namespace
{
using revia::evaluation::VerifyCalculation;
using revia::tests::Check;

void ExpectValue(const std::string& expression, const std::string& expected, const std::string& unit = "")
{
    const auto result = VerifyCalculation(nlohmann::json{{"expression", expression}, {"unit", unit}}.dump());
    Check(result.succeeded && result.value == expected && result.unit == unit,
        "Calculation failed: " + expression + " (" + result.refusal + ", " + result.value + ")");
    Check(!result.limitations.empty(), "Arithmetic result must state its premise limitations");
}

void ExpectRefusal(const std::string& expression)
{
    const auto result = VerifyCalculation(nlohmann::json{{"expression", expression}}.dump());
    Check(!result.succeeded && !result.refusal.empty() && result.value.empty(), "Unsafe calculation accepted: " + expression);
}
}

void RunCalculationVerifierTests()
{
    ExpectValue("2 + 3 * (4 - 1)", "11");
    ExpectValue("-(-2) + +.5", "2.5");
    ExpectValue("1e2 / 4", "25");
    ExpectValue("-0", "0");
    ExpectValue("2", "2", "min");
    ExpectValue("2 min + 5 s", "125", "s");
    ExpectValue("(2 km - 500 m) / 3", "500", "m");
    ExpectValue("2 * 500 g", "1", "kg");
    ExpectValue("2 min / 30 s", "4");
    ExpectValue("2 hours", "7200", "seconds");
    ExpectValue("100 cm", "1", "m");
    ExpectValue("1 m + 2 m", "3", "m");
    ExpectValue(std::string(32, '(') + "1" + std::string(32, ')'), "1");
    ExpectValue(std::string(128, '-') + "1", "1");
    for (const auto expression : {"1 / 0", "1e309", "1e308 * 10", "nan", "sqrt(4)", "1 s + 1 m", "2 m * 3 m", "1 / 2 s", "2 m / 1 s",
             "1;system(1)", "1 2", "()", "1 +", "1e", ".", "1 m + 1", "1 m / 0 m", "1e308 km", "2 m s"})
    {
        ExpectRefusal(expression);
    }
    for (const auto input :
        {"{\"expression\":\"1\",\"expression\":\"2\"}", "{\"expression\":2}", "{\"expression\":\"1\",\"extra\":true}", "[]",
            "{\"expression\":\"1\",\"unit\":\"USD\"}", "{\"expression\":\"2 m\",\"unit\":\"s\"}", "{\"expression\":\"1\",\"unit\":null}",
            "{\"expression\":\"1 m / 1 m\",\"unit\":\"m\"}", "{\"expression\":\"1\",\"unit\":\"s\",\"unit\":\"m\"}"})
    {
        Check(!VerifyCalculation(input).succeeded, "Invalid typed input accepted");
    }
    ExpectRefusal(std::string(33, '(') + "1" + std::string(33, ')'));
    ExpectRefusal(std::string(2049, '1'));
    ExpectRefusal(std::string(129, '-') + "1");
    std::string literals = "1";
    for (int index = 1; index < 65; ++index)
    {
        literals += "+1";
    }
    ExpectValue(literals.substr(0, literals.size() - 2), "64");
    ExpectRefusal(literals);
    Check(!VerifyCalculation(std::string(8193, ' ')).succeeded, "Oversized input accepted");
    std::string invalidUtf8 = "{\"expression\":\"";
    invalidUtf8 += static_cast<char>(0xFF);
    invalidUtf8 += "\"}";
    Check(!VerifyCalculation(invalidUtf8).succeeded, "Invalid UTF-8 accepted");
    std::stop_source cancellation;
    cancellation.request_stop();
    Check(!VerifyCalculation("{\"expression\":\"1+1\"}", cancellation.get_token()).succeeded, "Cancelled calculation accepted");
}
