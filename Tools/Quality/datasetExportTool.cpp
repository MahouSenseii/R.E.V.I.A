// Writes a reviewed training dataset export from a candidate file, through the existing
// Learning/datasetExport owner. Eligibility, grouping, partitions and withdrawal all stay
// there; this only reads the files and reports what it decided.

#include "Learning/datasetExport.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace
{
constexpr const char* Usage =
    "Usage: ReviaDatasetExport --candidates <file> --name <dataset> --salt <salt> --output <new directory> [options]\n"
    "  --withdrawn <file>          JSON array of withdrawn source IDs (default: none)\n"
    "  --prior <manifest.json>     the previous export's manifest, so its partitions are kept\n"
    "  --validation-percent <n>    default 10\n"
    "  --test-percent <n>          default 10\n";

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot read " + path.string() + '.');
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}

int main(const int argc, char** argv)
{
    using nlohmann::json;
    std::map<std::string, std::string> options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string flag = argv[index];
        if (!flag.starts_with("--") || index + 1 >= argc)
        {
            std::cerr << Usage;
            return 2;
        }
        options[flag.substr(2)] = argv[++index];
    }
    for (const char* required : {"candidates", "name", "salt", "output"})
        if (!options.contains(required))
        {
            std::cerr << Usage;
            return 2;
        }
    try
    {
        std::vector<revia::learning::DatasetCandidate> candidates;
        std::string error;
        if (!revia::learning::ReadDatasetCandidates(ReadAll(options["candidates"]), candidates, error))
            throw std::runtime_error(error);
        std::set<std::string> withdrawn;
        if (options.contains("withdrawn"))
            for (const auto& source : json::parse(ReadAll(options["withdrawn"])).get<std::vector<std::string>>())
                withdrawn.insert(source);
        const std::string prior = options.contains("prior") ? ReadAll(options["prior"]) : std::string{};
        revia::learning::DatasetExportOptions exportOptions{options["name"], options["salt"]};
        if (options.contains("validation-percent"))
            exportOptions.validationPercent = static_cast<unsigned>(std::stoul(options["validation-percent"]));
        if (options.contains("test-percent"))
            exportOptions.testPercent = static_cast<unsigned>(std::stoul(options["test-percent"]));
        revia::learning::DatasetExport exported;
        if (!revia::learning::BuildDatasetExport(candidates, withdrawn, exportOptions, prior, exported, error) ||
            !revia::learning::WriteDatasetExport(exported, options["output"], error))
            throw std::runtime_error(error);
        json summary = {{"state", "complete"}, {"output", options["output"]}, {"eligible", 0},
            {"partitions", {{"train", 0}, {"validation", 0}, {"test", 0}}}, {"excluded", json::array()}};
        for (const auto& decision : exported.decisions)
        {
            if (decision.eligible)
            {
                summary["eligible"] = summary["eligible"].get<int>() + 1;
                auto& count = summary["partitions"][revia::learning::ToString(decision.partition)];
                count = count.get<int>() + 1;
            }
            else
                summary["excluded"].push_back({{"id", decision.exampleId}, {"reason", decision.reason}});
        }
        std::cout << summary.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << json{{"state", "failed"}, {"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
