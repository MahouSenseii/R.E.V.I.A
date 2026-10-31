#include "Core/configManager.h"
#include "Core/appSettings.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"
#include "testSupport.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{
using nlohmann::json;
using revia::tests::Check;

std::string FlagValue(const std::vector<std::string>& arguments, const std::string& flag)
{
    const auto found = std::find(arguments.begin(), arguments.end(), flag);
    return found == arguments.end() || found + 1 == arguments.end() ? std::string("<absent>") : *(found + 1);
}

void TestPromptBatchConfiguration()
{
    revia::tests::ScopedTestDirectory directory;
    const auto path = directory.root / "settings.json";
    const json base = {{"activeProfile", "fixture"},
        {"llm", {{"backend", "None"}, {"autoStartServer", false}, {"visionEnabled", false}}},
        {"intelligence", {{"enabled", false}}}, {"embedding", {{"enabled", false}, {"autoStartServer", false}}},
        {"speechRecognition", {{"enabled", false}}}};
    const auto load = [&](const json& value, appSettings& result)
    {
        {
            std::ofstream file(path);
            file << value.dump();
        }
        return configManager(path.string(), directory.root.string()).LoadSettings(result);
    };
    const auto withBatch = [&base](const json& batch, const json& micro)
    {
        auto changed = base;
        if (!batch.is_discarded())
            changed["llm"]["batchSize"] = batch;
        if (!micro.is_discarded())
            changed["llm"]["microBatchSize"] = micro;
        return changed;
    };
    const json absent(json::value_t::discarded);

    appSettings legacy;
    Check(load(base, legacy) && legacy.llm.batchSize == 0 && legacy.llm.microBatchSize == 0,
        "A configuration without prompt batches changed llama.cpp's defaults.");
    for (const auto& [batch, micro] : {std::pair{2048, 512}, std::pair{1024, 1024}, std::pair{4096, 0}, std::pair{32, 32}})
    {
        appSettings loaded;
        Check(load(withBatch(batch, micro == 0 ? absent : json(micro)), loaded) && loaded.llm.batchSize == batch &&
                  loaded.llm.microBatchSize == micro,
            "A valid prompt batch of " + std::to_string(batch) + "/" + std::to_string(micro) + " was not loaded.");
    }

    // A micro-batch larger than its batch, or without one, would be shrunk silently by
    // llama.cpp, so the value asked for would not be the value that ran.
    for (const auto& [batch, micro] : {std::pair{absent, json(512)}, std::pair{json(1024), json(2048)},
             std::pair{json(0), json(512)}})
    {
        appSettings rejected;
        Check(!load(withBatch(batch, micro), rejected), "A micro-batch without a batch at least as large was accepted.");
    }
    for (const json& invalid : {json(-1), json(31), json(65537), json(1.5), json("2048"), json(true), json(nullptr),
             json(4294967296ULL)})
    {
        appSettings rejectedBatch;
        Check(!load(withBatch(invalid, absent), rejectedBatch), "An invalid batchSize was accepted: " + invalid.dump());
        appSettings rejectedMicro;
        Check(!load(withBatch(json(65536), invalid), rejectedMicro),
            "An invalid microBatchSize was accepted: " + invalid.dump());
    }
}

void TestPromptBatchLaunchArguments()
{
    llmSettings settings;
    settings.contextSize = 32768;
    settings.kvCacheType = "q8_0";
    const auto unchanged = BuildLlamaServerArguments(settings, false, "", "");
    Check(FlagValue(unchanged, "--batch-size") == "<absent>" && FlagValue(unchanged, "--ubatch-size") == "<absent>",
        "An unset prompt batch changed llama.cpp's default.");

    settings.batchSize = 4096;
    const auto logicalOnly = BuildLlamaServerArguments(settings, false, "", "");
    Check(FlagValue(logicalOnly, "--batch-size") == "4096" && FlagValue(logicalOnly, "--ubatch-size") == "<absent>",
        "A batch without a micro-batch did not reach llama-server on its own.");

    settings.batchSize = 2048;
    settings.microBatchSize = 1024;
    auto both = BuildLlamaServerArguments(settings, false, "", "");
    Check(FlagValue(both, "--batch-size") == "2048" && FlagValue(both, "--ubatch-size") == "1024",
        "The configured prompt batches did not reach llama-server.");
    for (const std::string flag : {"--batch-size", "--ubatch-size"})
        both.erase(std::find(both.begin(), both.end(), flag), std::find(both.begin(), both.end(), flag) + 2);
    Check(both == unchanged, "Configuring prompt batches changed another llama-server argument.");

    // The embedding server sizes its own batch to hold a whole recall query.
    llmSettings embedding;
    embedding.contextSize = 2048;
    embedding.batchSize = 4096;
    embedding.microBatchSize = 256;
    const auto embedded = BuildLlamaServerArguments(embedding, true, "mean", "none");
    Check(FlagValue(embedded, "--batch-size") == "2048" && FlagValue(embedded, "--ubatch-size") == "2048" &&
              std::count(embedded.begin(), embedded.end(), "--batch-size") == 1,
        "Chat prompt batches reached the embedding server.");
}

void TestContextBatchLogParsing()
{
    // Lines in the shape llama.cpp's context constructor prints them.
    const std::string current =
        "llama_model_loader: - kv  20: general.name str = Qwen3.5 9B\n"
        "load_tensors: offloaded 33/33 layers to GPU\n"
        "llama_context: constructing llama_context\n"
        "llama_context: n_seq_max             = 1\n"
        "llama_context: n_ctx                 = 32768\n"
        "llama_context: n_batch               = 2048\n"
        "llama_context: n_ubatch              = 512\n"
        "llama_context: causal_attn           = 1\n";
    const auto parsed = ParseLlamaContextBatch(current);
    Check(parsed && parsed->logical == 2048 && parsed->physical == 512, "The current llama.cpp context log was not read.");

    const auto older = ParseLlamaContextBatch("llama_new_context_with_model: n_batch    = 1024\r\n"
                                              "llama_new_context_with_model: n_ubatch   = 256\r\n");
    Check(older && older->logical == 1024 && older->physical == 256, "The older llama.cpp context log was not read.");

    const auto targetFirst = ParseLlamaContextBatch(current + "llama_context: n_batch = 512\nllama_context: n_ubatch = 128\n");
    Check(targetFirst && targetFirst->logical == 2048 && targetFirst->physical == 512,
        "A later context, such as a draft model's, replaced the target's batches.");

    for (const std::string& unreported :
        {std::string(), std::string("llama_context: n_batch = 2048\n"), std::string("llama_context: n_ubatch = 512\n"),
            std::string("common_init_from_params: n_batch = 2048\ncommon_init_from_params: n_ubatch = 512\n"),
            std::string("llama_context: n_batch = 123456789\nllama_context: n_ubatch = 512\n"),
            std::string("llama_context: n_batch = \nllama_context: n_ubatch = 512\n")})
        Check(!ParseLlamaContextBatch(unreported), "An incomplete or foreign log line was reported as the context's batch.");

    const llamaCppServerProcess neverLaunched;
    Check(!neverLaunched.LaunchedPromptBatch(), "An owner that launched nothing reported a prompt batch.");
}
}

void RunPromptBatchSettingsTests()
{
    std::string errors;
    for (const auto test : {TestPromptBatchConfiguration, TestPromptBatchLaunchArguments, TestContextBatchLogParsing})
        try
        {
            test();
        }
        catch (const std::exception& error)
        {
            errors += std::string(error.what()) + '\n';
        }
    Check(errors.empty(), errors);
    std::cout << "Prompt batch settings, launch arguments and context log reading passed.\n";
}
