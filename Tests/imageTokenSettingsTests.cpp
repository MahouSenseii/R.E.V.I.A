#include "Core/configManager.h"
#include "Core/appSettings.h"
#include "Core/settingsFileEditor.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"
#include "LLM/imageTokenBudget.h"
#include "testSupport.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace
{
using nlohmann::json;
using revia::tests::Check;

void TestImageMinimumConfiguration()
{
    revia::tests::ScopedTestDirectory directory;
    const auto path = directory.root / "settings.json";
    json settings = {{"activeProfile", "fixture"},
        {"llm", {{"backend", "None"}, {"autoStartServer", false}, {"visionEnabled", false}, {"imageMinTokens", 1024}}},
        {"intelligence", {{"enabled", false}, {"expert", {{"enabled", false}, {"imageMinTokens", 512}}}}},
        {"embedding", {{"enabled", false}, {"autoStartServer", false}}}, {"speechRecognition", {{"enabled", false}}}};
    const auto load = [&](const json& value, appSettings& result)
    {
        {
            std::ofstream file(path);
            file << value.dump();
        }
        return configManager(path.string(), directory.root.string()).LoadSettings(result);
    };
    appSettings loaded;
    Check(load(settings, loaded) && loaded.llm.imageMinTokens == 1024 && loaded.intelligence.expert.imageMinTokens == 512,
        "Configured per-model image minima were not loaded.");
    for (const json invalid : {json(-1), json(4097), json(1.5), json("1024"), json(true), json(4294967296ULL)})
        for (const bool tier : {false, true})
        {
            auto changed = settings;
            (tier ? changed["intelligence"]["expert"] : changed["llm"])["imageMinTokens"] = invalid;
            appSettings rejected;
            Check(!load(changed, rejected), "An invalid or non-integer image minimum was accepted.");
        }
    settings["llm"].erase("imageMinTokens");
    settings["intelligence"]["expert"].erase("imageMinTokens");
    appSettings legacy;
    Check(load(settings, legacy) && legacy.llm.imageMinTokens == 0 && legacy.intelligence.expert.imageMinTokens == 0,
        "A legacy configuration silently changed backend image sizing.");
}

void TestImageMinimumLaunchArguments()
{
    llmSettings settings;
    settings.bVisionEnabled = true;
    settings.imageMinTokens = 1024;
    const auto args = BuildLlamaServerArguments(settings, false, "", "");
    const auto flag = std::find(args.begin(), args.end(), "--image-min-tokens");
    Check(flag != args.end() && flag + 1 != args.end() && *(flag + 1) == "1024", "The bounded image minimum did not reach llama-server.");
    for (const bool embedding : {false, true})
    {
        settings.bVisionEnabled = false;
        const auto textOnly = BuildLlamaServerArguments(settings, embedding, "mean", "none");
        Check(std::find(textOnly.begin(), textOnly.end(), "--image-min-tokens") == textOnly.end(),
            "A text-only or embedding server received a vision-only image minimum.");
    }
    settings.bVisionEnabled = true;
    settings.imageMinTokens = 0;
    const auto unchanged = BuildLlamaServerArguments(settings, false, "", "");
    Check(std::find(unchanged.begin(), unchanged.end(), "--image-min-tokens") == unchanged.end(), "Zero changed the backend default.");
}

void TestImageMinimumContextReservation()
{
    using revia::llm::AttachmentImageTokenAllowance;
    Check(AttachmentImageTokenAllowance(4, 3, 1024) >= 1024 && AttachmentImageTokenAllowance(4, 3, 0) == 17,
        "Image context fitting did not reserve the configured per-image floor or preserve the default.");
    Check(AttachmentImageTokenAllowance(2048, 2048, 1024) == 4096 && AttachmentImageTokenAllowance(28, 28, 4096) == 4096,
        "Image context fitting lost its existing 4096-token ceiling.");
    Check(AttachmentImageTokenAllowance(640, 440, 1024) >= 1053 + 16, "The measured aspect-rounded image token cost was underreserved.");
}
}

void RunImageTokenSettingsTests()
{
    std::string errors;
    for (const auto test : {TestImageMinimumConfiguration, TestImageMinimumLaunchArguments, TestImageMinimumContextReservation})
        try
        {
            test();
        }
        catch (const std::exception& error)
        {
            errors += std::string(error.what()) + '\n';
        }
    Check(errors.empty(), errors);
    std::cout << "Image minimum settings, launch arguments and context reservations passed.\n";
}
