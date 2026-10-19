#include "Core/appSettings.h"
#include "Core/atomicFile.h"
#include "Core/configManager.h"
#include "Core/settingsFileEditor.h"
#include "testSupport.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace
{
using json = nlohmann::json;
using revia::core::SettingsFileEditor;
using revia::tests::Check;

void Write(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    output.close();
    Check(!output.fail(), "Could not write a settings editor fixture.");
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

bool Mentions(const std::vector<std::string>& warnings, const std::string& text)
{
    return std::any_of(warnings.begin(), warnings.end(), [&text](const std::string& warning) { return warning.find(text) != std::string::npos; });
}

// A settings file that loads, with two MCP servers and no vision projector, so turning on
// picture understanding is a change it cannot accept.
json BaseSettings(const std::filesystem::path& root)
{
    return {{"activeProfile", "fixture"},
        {"llm", {{"backend", "None"}, {"autoStartServer", false}, {"visionEnabled", false}, {"modelPath", (root / "absent.gguf").string()},
                    {"multimodalProjectorPath", ""}, {"mediaPath", (root / "Vision").string()}}},
        {"intelligence", {{"enabled", false}}}, {"embedding", {{"enabled", false}, {"autoStartServer", false}}},
        {"speech", {{"enabled", true}, {"backend", "WindowsSapi"}, {"voiceDataPath", (root / "Voices").string()}}},
        {"speechRecognition", {{"enabled", false}}}, {"vision", {{"enabled", true}}}, {"perception", {{"enabled", false}}},
        {"mcp", {{"servers", json::array({{{"id", "files"}, {"transport", "http"}, {"url", "http://127.0.0.1:9/files"}},
                                 {{"id", "notes"}, {"transport", "http"}, {"url", "http://127.0.0.1:9/notes"}}})}}}};
}

// Everything an owner key can switch to is valid here, so each one can be flipped both ways.
json PermissiveSettings(const std::filesystem::path& root)
{
    json settings = BaseSettings(root);
    settings["llm"]["multimodalProjectorPath"] = (root / "projector.gguf").string();
    settings["llm"]["serverExecutable"] = (root / "llama-server.exe").string();
    settings["llm"]["cloud"] = {{"baseUrl", "https://cloud.example/v1"}, {"model", "fixture-model"}};
    settings["intelligence"] = {{"enabled", true}};
    settings["embedding"] = {{"enabled", true}, {"autoStartServer", true}, {"serverExecutable", (root / "embed-server.exe").string()},
        {"modelPath", (root / "embed.gguf").string()}};
    return settings;
}

struct Fixture
{
    revia::tests::ScopedTestDirectory directory;
    std::filesystem::path settings;
    std::filesystem::path profiles;
    std::filesystem::path overlay;
    std::string original;

    explicit Fixture(const bool permissive = false)
        : settings(directory.root / "Config/settings.json"), profiles(directory.root / "Config/Profiles"),
          overlay(directory.root / "RuntimeData/Settings/owner-settings.json")
    {
        Write(settings, (permissive ? PermissiveSettings(directory.root) : BaseSettings(directory.root)).dump(2));
        original = Read(settings);
    }

    [[nodiscard]] SettingsFileEditor Editor() const { return SettingsFileEditor(overlay, settings.string(), profiles.string()); }

    [[nodiscard]] appSettings Load(std::vector<std::string>* warnings = nullptr) const
    {
        configManager config(settings.string(), profiles.string());
        config.SetOwnerOverlay(overlay.string());
        appSettings loaded;
        std::vector<std::string> ignored;
        Check(config.LoadSettings(loaded, warnings ? *warnings : ignored), "The fixture settings with the owner overlay did not load.");
        return loaded;
    }

    // The property the overlay exists for: the build owns Config/settings.json.
    void CheckBaseUntouched() const
    {
        Check(Read(settings) == original, "The owner settings editor modified Config/settings.json.");
    }
};

void TestAllowlistIsEnforced()
{
    Fixture fixture;
    SettingsFileEditor editor = fixture.Editor();
    std::string error;
    Check(!editor.SetBool("llm.port", true, error) && !error.empty(), "A key outside the allowlist was written.");
    Check(!editor.SetBool("activeProfile", true, error), "activeProfile was written through the owner overlay.");
    Check(!editor.SetBool("llm.cloud.useFor", true, error), "A choice key was written as on/off.");
    Check(!editor.SetChoice("vision.enabled", "on", error), "An on/off key was written as a choice.");
    Check(!editor.SetChoice("llm.cloud.useFor", "sometimes", error), "A choice outside its allowed values was written.");
    Check(!editor.SetBool("mcp.servers[Files].enabled", true, error) && !editor.SetBool("mcp.servers[files].command", true, error),
        "An MCP key with a bad id or field was accepted.");
    Check(!std::filesystem::exists(fixture.overlay), "A refused key still created the overlay file.");
    Check(revia::core::FindOwnerKey("vision.enabled") && !revia::core::FindOwnerKey("vision.requireConfirmation"),
        "The allowlist lookup does not match settingsFileKeys.h.");

    // The reader applies the same allowlist to a hand-edited file.
    json data = BaseSettings(fixture.directory.root);
    std::vector<std::string> warnings;
    Check(revia::core::ApplyOwnerOverrides(data,
              {{"schemaVersion", 1}, {"values", {{"llm.port", 1}, {"vision.enabled", "yes"}, {"vision.continuousAwareness", true}}}},
              warnings),
        "A usable overlay was refused as a whole.");
    Check(Mentions(warnings, "\"llm.port\"") && Mentions(warnings, "\"vision.enabled\"") && warnings.size() == 2,
        "Unknown or wrongly typed overlay entries were not reported.");
    Check(!data["llm"].contains("port") && data["vision"]["enabled"] == true && data["vision"]["continuousAwareness"] == true,
        "The overlay applied a refused entry or skipped an allowed one.");
    fixture.CheckBaseUntouched();
}

void TestValidationRefusalKeepsThePreviousOverlay()
{
    Fixture fixture;
    SettingsFileEditor editor = fixture.Editor();
    std::string error;
    Check(editor.SetBool("vision.enabled", false, error), "A valid owner change was refused: " + error);
    const std::string saved = Read(fixture.overlay);
    Check(!editor.SetBool("llm.visionEnabled", true, error) && !error.empty(),
        "Picture understanding was saved although settings.json has no vision projector.");
    Check(Read(fixture.overlay) == saved, "A refused change altered the saved overlay.");
    Check(!std::filesystem::exists(fixture.overlay.string() + ".pending"), "A refused change left its pending file behind.");
    const appSettings loaded = fixture.Load();
    Check(!loaded.vision.bEnabled && !loaded.llm.bVisionEnabled, "The overlay after a refusal does not hold exactly the accepted change.");
    fixture.CheckBaseUntouched();
}

void TestAWrongPathIsRefused()
{
    // "vision" is a value here, so vision.enabled cannot reach the field configManager
    // reads. The candidate still loads, without the new value, and that is refused.
    Fixture fixture;
    json settings = BaseSettings(fixture.directory.root);
    settings["vision"] = true;
    Write(fixture.settings, settings.dump(2));
    fixture.original = Read(fixture.settings);
    SettingsFileEditor editor = fixture.Editor();
    std::string error;
    Check(!editor.SetBool("vision.enabled", false, error) && error.find("did not reach") != std::string::npos,
        "A change that never reached its setting was saved: " + error);
    Check(!std::filesystem::exists(fixture.overlay), "A change that never reached its setting created the overlay.");
    fixture.CheckBaseUntouched();
}

void TestMcpServersAreAddressedById()
{
    Fixture fixture;
    SettingsFileEditor editor = fixture.Editor();
    std::string error;
    Check(editor.SetBool("mcp.servers[notes].enabled", true, error), "Enabling an MCP server by id failed: " + error);
    Check(editor.SetBool("mcp.servers[files].requireConfirmation", false, error), "Changing MCP confirmation by id failed: " + error);
    const appSettings loaded = fixture.Load();
    Check(loaded.mcp.servers.size() == 2 && loaded.mcp.servers[0].id == "files" && loaded.mcp.servers[1].id == "notes",
        "The overlay changed the MCP server list.");
    Check(!loaded.mcp.servers[0].bEnabled && loaded.mcp.servers[1].bEnabled, "The MCP enable change reached the wrong server.");
    Check(!loaded.mcp.servers[0].bRequireConfirmation && loaded.mcp.servers[1].bRequireConfirmation,
        "The MCP confirmation change reached the wrong server.");
    Check(revia::core::OwnerSettingValue(loaded, "mcp.servers[notes].enabled") == json(true),
        "The MCP owner value does not read back by id.");
    fixture.CheckBaseUntouched();
}

void TestAMissingServerIsSkippedWithAWarning()
{
    Fixture fixture;
    json data = BaseSettings(fixture.directory.root);
    const json before = data;
    std::vector<std::string> warnings;
    Check(revia::core::ApplyOwnerOverrides(data, {{"schemaVersion", 1}, {"values", {{"mcp.servers[ghost].enabled", true}}}}, warnings),
        "An overlay naming a missing server was refused as a whole.");
    Check(data == before && Mentions(warnings, "ghost"), "A missing MCP server was not skipped with a warning.");

    std::string error;
    Check(!fixture.Editor().SetBool("mcp.servers[ghost].enabled", true, error), "A change for a missing MCP server was saved.");

    // A server renamed in settings.json after the owner chose: startup keeps going.
    Write(fixture.overlay, R"({"schemaVersion":1,"values":{"mcp.servers[ghost].enabled":true,"vision.enabled":false}})");
    std::vector<std::string> loadWarnings;
    const appSettings loaded = fixture.Load(&loadWarnings);
    Check(Mentions(loadWarnings, "ghost") && !loaded.vision.bEnabled, "A stale MCP entry stopped the rest of the overlay.");
    fixture.CheckBaseUntouched();
}

void TestAnUnusableOverlayIsIgnored()
{
    Fixture fixture;
    Write(fixture.overlay, "{not json");
    std::vector<std::string> warnings;
    appSettings loaded = fixture.Load(&warnings);
    Check(!warnings.empty() && loaded.vision.bEnabled, "An unparseable overlay was not ignored with a warning.");

    Write(fixture.overlay, R"({"schemaVersion":2,"values":{"vision.enabled":false}})");
    warnings.clear();
    loaded = fixture.Load(&warnings);
    Check(!warnings.empty() && loaded.vision.bEnabled, "An overlay of an unknown schema was applied.");

    // Written by hand past the editor: the one entry settings.json can no longer accept is
    // set aside, and the owner's other choice is kept rather than dropped with it.
    Write(fixture.overlay, R"({"schemaVersion":1,"values":{"llm.visionEnabled":true,"vision.enabled":false}})");
    warnings.clear();
    loaded = fixture.Load(&warnings);
    Check(!loaded.llm.bVisionEnabled && !loaded.vision.bEnabled && Mentions(warnings, "llm.visionEnabled"),
        "A stale overlay entry was not set aside on its own.");

    // With the stale entry present, the editor can still return a key to settings.json.
    std::string error;
    Check(fixture.Editor().Remove("llm.visionEnabled", error), "The stale entry could not be removed: " + error);
    fixture.CheckBaseUntouched();
}

void TestAStaleEntryDoesNotBlockOtherChanges()
{
    // Startup sets the stale entry aside and loads, so it must not stop the owner changing,
    // or resetting, anything else.
    Fixture fixture;
    Write(fixture.overlay, R"({"schemaVersion":1,"values":{"llm.visionEnabled":true,"image.keepLoaded":true}})");
    SettingsFileEditor editor = fixture.Editor();
    std::string error;
    Check(editor.SetBool("vision.enabled", false, error), "A stale entry blocked an unrelated change: " + error);
    Check(editor.Remove("image.keepLoaded", error), "A stale entry blocked resetting an unrelated key: " + error);
    Check(editor.SetChoice("llm.cloud.useFor", "never", error), "A stale entry blocked an unrelated choice: " + error);
    Check(editor.SetBool("mcp.servers[files].enabled", false, error), "A stale entry blocked an MCP change: " + error);
    Check(editor.Value("llm.visionEnabled") == json(true), "An unrelated change dropped the owner's stale choice.");
    std::vector<std::string> warnings;
    const appSettings loaded = fixture.Load(&warnings);
    Check(!loaded.vision.bEnabled && !loaded.image.bKeepLoaded && !loaded.mcp.servers[0].bEnabled && loaded.llm.cloud.useFor == "never" &&
              !loaded.llm.bVisionEnabled && Mentions(warnings, "llm.visionEnabled"),
        "The changes made beside a stale entry did not load as written.");

    // Writing the stale entry again as it was still cannot reach its field, and says why.
    Check(!editor.SetBool("llm.visionEnabled", true, error) && error.find("llm.visionEnabled") != std::string::npos,
        "Re-saving a choice settings.json cannot accept was not refused by name: " + error);
    Check(editor.SetBool("llm.visionEnabled", false, error), "The stale entry could not be corrected: " + error);
    warnings.clear();
    static_cast<void>(fixture.Load(&warnings));
    Check(warnings.empty(), "Correcting the stale entry left a warning behind.");

    // A change that would set something aside names it.
    Fixture fresh;
    SettingsFileEditor freshEditor = fresh.Editor();
    Check(!freshEditor.SetBool("llm.visionEnabled", true, error) && error.find("\"llm.visionEnabled\"") != std::string::npos,
        "A refused change did not name the entry it would set aside: " + error);
    Check(!std::filesystem::exists(fresh.overlay), "A refused first change created the overlay.");
    fixture.CheckBaseUntouched();
    fresh.CheckBaseUntouched();
}

void TestAnUnreadableOverlayIsMovedAside()
{
    // Every load ignores these files. The next change keeps them under a new name instead of
    // overwriting them, and never brings back a value the load had refused.
    for (const std::string& unreadable : {std::string("{not json"),
             std::string(R"({"schemaVersion":2,"values":{"vision.enabled":false,"image.keepLoaded":true}})")})
    {
        Fixture fixture;
        Write(fixture.overlay, unreadable);
        SettingsFileEditor editor = fixture.Editor();
        std::string error;
        Check(editor.SetBool("vision.continuousAwareness", true, error), "A change over an unusable overlay failed: " + error);
        Check(editor.Notice().find("unreadable-") != std::string::npos, "Moving an unusable overlay aside was not reported.");
        std::vector<std::filesystem::path> aside;
        for (const auto& entry : std::filesystem::directory_iterator(fixture.overlay.parent_path()))
        {
            if (entry.path().filename().string().starts_with("owner-settings.json.unreadable-"))
                aside.push_back(entry.path());
        }
        Check(aside.size() == 1 && Read(aside.front()) == unreadable, "The unusable overlay was not kept under its own name.");
        const json saved = json::parse(Read(fixture.overlay));
        Check(saved["schemaVersion"] == 1 && saved["values"] == json({{"vision.continuousAwareness", true}}),
            "The new overlay carried over values a load had refused.");
        const appSettings loaded = fixture.Load();
        Check(loaded.vision.bEnabled && !loaded.image.bKeepLoaded && loaded.vision.bContinuousAwareness,
            "Settings from an unusable overlay took effect after the next change.");
        Check(editor.SetBool("vision.enabled", false, error) && editor.Notice().empty(), "The fresh overlay was not used: " + error);
        fixture.CheckBaseUntouched();
    }
}

void TestRemoveReturnsToSettingsJson()
{
    Fixture fixture;
    SettingsFileEditor editor = fixture.Editor();
    std::string error;
    Check(editor.SetChoice("llm.cloud.useFor", "ask", error) && editor.Value("llm.cloud.useFor") == json("ask"),
        "A choice was not stored: " + error);
    Check(fixture.Load().llm.cloud.useFor == "ask", "A stored choice did not reach the loaded settings.");
    Check(editor.Remove("llm.cloud.useFor", error) && !editor.Value("llm.cloud.useFor"), "Remove kept the key: " + error);
    Check(fixture.Load().llm.cloud.useFor == appSettings{}.llm.cloud.useFor, "Remove did not return to the settings.json value.");
    Check(editor.Remove("llm.cloud.useFor", error), "Removing a key that is not set failed: " + error);
    fixture.CheckBaseUntouched();
}

void TestTheReplacedOverlayIsKeptAsBak()
{
    Fixture fixture;
    SettingsFileEditor editor = fixture.Editor();
    const std::filesystem::path backup = fixture.overlay.string() + ".bak";
    std::string error;
    Check(editor.SetBool("vision.enabled", false, error) && !std::filesystem::exists(backup), "The first write invented a backup.");
    const std::string first = Read(fixture.overlay);
    Check(editor.SetBool("image.keepLoaded", true, error), "A second owner change failed: " + error);
    Check(std::filesystem::exists(backup) && Read(backup) == first, "The previous overlay was not kept as owner-settings.json.bak.");

    // The shared helper on its own.
    const std::filesystem::path target = fixture.directory.root / "atomic.txt";
    const std::filesystem::path pending = fixture.directory.root / "atomic.txt.pending";
    Write(pending, "one");
    Check(revia::core::ReplaceFileAtomically(target, pending, error) && Read(target) == "one" && !std::filesystem::exists(pending),
        "ReplaceFileAtomically did not move a new file in: " + error);
    Write(pending, "two");
    Check(revia::core::ReplaceFileAtomically(target, pending, error) && Read(target) == "two" &&
              Read(fixture.directory.root / "atomic.txt.bak") == "one",
        "ReplaceFileAtomically did not keep the replaced file: " + error);
    fixture.CheckBaseUntouched();
}

void TestEveryOwnerKeyReachesItsField()
{
    // Each allowlisted path must be the one configManager reads, both ways. A key whose path
    // is wrong is refused by the editor's read-back, so it shows up here as a failed write.
    Fixture fixture(true);
    SettingsFileEditor editor = fixture.Editor();
    const appSettings base = fixture.Load();
    for (const revia::core::OwnerKey& entry : revia::core::kOwnerKeys)
    {
        std::vector<std::string> keys;
        if (entry.key.starts_with("mcp."))
        {
            const std::string field(entry.key.substr(entry.key.rfind('.')));
            keys = {"mcp.servers[files]" + field, "mcp.servers[notes]" + field};
        }
        else
        {
            keys = {std::string(entry.key)};
        }
        for (const std::string& key : keys)
        {
            const auto configured = revia::core::OwnerSettingValue(base, key);
            Check(configured.has_value(), "No settings field reads owner key " + key + ".");
            std::vector<json> values;
            if (entry.type == revia::core::OwnerKeyType::Bool)
                values = {json(!configured->get<bool>()), *configured};
            else
                values = {json("never"), json("ask"), json("hardTurns")};
            for (const json& value : values)
            {
                std::string error;
                const bool written = value.is_boolean() ? editor.SetBool(key, value.get<bool>(), error)
                                                        : editor.SetChoice(key, value.get<std::string>(), error);
                Check(written, "Owner key " + key + " could not be set to " + value.dump() + ": " + error);
                Check(revia::core::OwnerSettingValue(fixture.Load(), key) == value, "Owner key " + key + " did not load as written.");
            }
            std::string error;
            Check(editor.Remove(key, error) && revia::core::OwnerSettingValue(fixture.Load(), key) == configured,
                "Owner key " + key + " did not return to settings.json: " + error);
        }
    }
    fixture.CheckBaseUntouched();
}
} // namespace

void RunSettingsFileEditorTests()
{
    TestAllowlistIsEnforced();
    TestValidationRefusalKeepsThePreviousOverlay();
    TestAWrongPathIsRefused();
    TestMcpServersAreAddressedById();
    TestAMissingServerIsSkippedWithAWarning();
    TestAnUnusableOverlayIsIgnored();
    TestAStaleEntryDoesNotBlockOtherChanges();
    TestAnUnreadableOverlayIsMovedAside();
    TestRemoveReturnsToSettingsJson();
    TestTheReplacedOverlayIsKeptAsBak();
    TestEveryOwnerKeyReachesItsField();
    std::cout << "Settings file editor tests passed.\n";
}
