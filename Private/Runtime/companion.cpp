#include "Runtime/companion.h"
#include "Identity/identityStore.h"
#include "Core/configManager.h"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace revia::runtime
{
namespace
{
using json = nlohmann::json;
const CompanionDescriptor Legacy{"legacy", "Revia", "assistant", true};
bool SafeId(const std::string& id)
{
    static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$");
    static const std::regex reserved("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$", std::regex::icase);
    return std::regex_match(id, pattern) && !std::regex_match(id, reserved);
}
bool SafeProfile(const std::string& profile)
{
    static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$");
    return std::regex_match(profile, pattern) && configManager::IsSafeProfileId(profile) && profile.back() != '.';
}

bool Inside(const std::filesystem::path& root, const std::filesystem::path& path)
{
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

void CheckPath(const std::filesystem::path& path)
{
    std::filesystem::path cursor;
    for (const auto& part : path)
    {
        cursor /= part;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(cursor.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::invalid_argument("Companion paths cannot cross a reparse point.");
#endif
        std::error_code error;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(cursor, error)))
            throw std::invalid_argument("Companion paths cannot cross a symbolic link.");
        if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory)
            throw std::invalid_argument("Companion path could not be inspected.");
    }
}

bool AtomicJson(const std::filesystem::path& path, const json& data, std::string& error)
{
    try
    {
        CheckPath(path);
        std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        CheckPath(temporary);
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << data.dump(2) << '\n';
        stream.flush();
        if (!stream)
            throw std::runtime_error("Companion metadata could not be written.");
        stream.close();
        if (stream.fail())
            throw std::runtime_error("Companion metadata could not be closed.");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Companion metadata could not be published.");
#else
        std::filesystem::rename(temporary, path);
#endif
        return true;
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
        return false;
    }
}

class Digest
{
  public:
    Digest()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
            throw std::runtime_error("Companion integrity hash could not be initialized.");
    }
    ~Digest()
    {
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    void Add(const void* bytes, std::size_t size)
    {
        const auto* data = static_cast<const unsigned char*>(bytes);
        while (size)
        {
            const auto count = static_cast<ULONG>(std::min<std::size_t>(size, 65536));
            if (BCryptHashData(hash, const_cast<PUCHAR>(data), count, 0) < 0)
                throw std::runtime_error("Companion integrity hash failed.");
            data += count;
            size -= count;
        }
    }
    void Text(const std::string& value)
    {
        const auto size = static_cast<std::uint64_t>(value.size());
        Add(&size, sizeof(size));
        Add(value.data(), value.size());
    }
    std::string Finish()
    {
        std::array<unsigned char, 32> output{};
        if (BCryptFinishHash(hash, output.data(), output.size(), 0) < 0)
            throw std::runtime_error("Companion integrity hash could not be finished.");
        std::ostringstream result;
        for (const auto byte : output)
            result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        return result.str();
    }

  private:
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
};

std::string FileDigest(const std::filesystem::path& path)
{
    CheckPath(path);
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("A migration artifact could not be read.");
    Digest digest;
    std::array<char, 65536> buffer{};
    while (stream)
    {
        stream.read(buffer.data(), buffer.size());
        digest.Add(buffer.data(), static_cast<std::size_t>(stream.gcount()));
    }
    if (!stream.eof())
        throw std::runtime_error("A migration artifact could not be read completely.");
    return digest.Finish();
}

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Database OpenDatabase(const std::filesystem::path& path, int flags)
{
    sqlite3* raw = nullptr;
    const auto code = sqlite3_open_v2(path.string().c_str(), &raw, flags | SQLITE_OPEN_FULLMUTEX, nullptr);
    Database database(raw, sqlite3_close);
    if (code != SQLITE_OK)
        throw std::runtime_error("A migration database could not be opened.");
    sqlite3_busy_timeout(raw, 1000);
    return database;
}
Statement Query(sqlite3* database, const std::string& sql)
{
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("A migration database schema could not be inspected.");
    return Statement(raw, sqlite3_finalize);
}
std::string Column(sqlite3_stmt* query, int column)
{
    const auto* text = sqlite3_column_text(query, column);
    return text ? std::string(reinterpret_cast<const char*>(text), sqlite3_column_bytes(query, column)) : std::string{};
}
std::string Quote(const std::string& name)
{
    std::string result = "\"";
    for (char c : name)
    {
        result += c;
        if (c == '"')
            result += c;
    }
    return result + '"';
}

std::string DatabaseDigest(sqlite3* database)
{
    auto integrity = Query(database, "PRAGMA integrity_check");
    if (sqlite3_step(integrity.get()) != SQLITE_ROW || Column(integrity.get(), 0) != "ok" || sqlite3_step(integrity.get()) != SQLITE_DONE)
        throw std::runtime_error("A migration database failed its integrity check.");
    auto version = Query(database, "PRAGMA user_version");
    if (sqlite3_step(version.get()) != SQLITE_ROW || sqlite3_column_int(version.get(), 0) != 0)
        throw std::runtime_error("A migration database has an unsupported schema version.");
    Digest digest;
    auto schema = Query(database, "SELECT type,name,tbl_name,coalesce(sql,'') FROM sqlite_master ORDER BY type,name");
    std::vector<std::string> tables;
    int code;
    while ((code = sqlite3_step(schema.get())) == SQLITE_ROW)
    {
        for (int i = 0; i < 4; ++i)
            digest.Text(Column(schema.get(), i));
        if (Column(schema.get(), 0) == "table")
            tables.push_back(Column(schema.get(), 1));
    }
    if (code != SQLITE_DONE || tables.empty())
        throw std::runtime_error("A migration database has no usable schema.");
    for (const auto& table : tables)
    {
        digest.Text(table);
        auto columns = Query(database, "PRAGMA table_info(" + Quote(table) + ")");
        std::string order;
        while (sqlite3_step(columns.get()) == SQLITE_ROW)
        {
            if (!order.empty())
                order += ',';
            order += Quote(Column(columns.get(), 1));
        }
        if (order.empty())
            throw std::runtime_error("A migration database table has no columns.");
        auto rows = Query(database, "SELECT * FROM " + Quote(table) + " ORDER BY " + order);
        while ((code = sqlite3_step(rows.get())) == SQLITE_ROW)
        {
            for (int i = 0; i < sqlite3_column_count(rows.get()); ++i)
            {
                const int type = sqlite3_column_type(rows.get(), i);
                digest.Add(&type, sizeof(type));
                if (type == SQLITE_BLOB)
                {
                    const auto size = static_cast<std::uint64_t>(sqlite3_column_bytes(rows.get(), i));
                    digest.Add(&size, sizeof(size));
                    digest.Add(sqlite3_column_blob(rows.get(), i), size);
                }
                else
                    digest.Text(Column(rows.get(), i));
            }
        }
        if (code != SQLITE_DONE)
            throw std::runtime_error("A migration database could not be validated completely.");
    }
    return digest.Finish();
}
bool IsDatabase(const std::filesystem::path& path)
{
    return path.extension() == ".db";
}

json ParseWholeDocument(std::istream& input)
{
    auto value = json::parse(input);
    // The parser accepts a raw NUL as a terminator; only the stream's actual EOF closes a saved document.
    if (!input.eof())
        throw std::runtime_error("A JSON document has trailing content or could not be read completely.");
    return value;
}

void ValidateDocument(const std::filesystem::path& path, const std::filesystem::path& relative)
{
    if (relative == "RuntimeData/Identity/identity.json")
    {
        revia::identity::IdentityStore store(path.string());
        revia::identity::IdentitySnapshot identity;
        std::string error;
        if (!store.Load(identity, error))
            throw std::runtime_error("The migration identity is invalid or newer than this build.");
    }
    if (path.extension() == ".json" || path.extension() == ".jsonl")
    {
        std::ifstream input(path);
        if (!input)
            throw std::runtime_error("A migration document could not be read.");
        const bool identityDocument = path.filename() == "identity.json" && path.parent_path().filename() == "Identity";
        const auto validate = [identityDocument](const json& item)
        {
            if (!item.is_object() && !item.is_array())
                throw std::runtime_error("A migration document has an invalid structure.");
            if (item.is_object() && item.contains("schemaVersion"))
            {
                const int maximumVersion = identityDocument ? identity::IdentitySchemaVersion : 2;
                if (!item["schemaVersion"].is_number_integer() || item["schemaVersion"].get<int>() > maximumVersion ||
                    item["schemaVersion"].get<int>() < 0)
                    throw std::runtime_error("A migration document has an unsupported schema version.");
            }
        };
        if (path.extension() == ".json")
        {
            const auto item = ParseWholeDocument(input);
            validate(item);
        }
        else
        {
            std::string line;
            while (std::getline(input, line))
                if (!line.empty())
                {
                    std::istringstream record(line);
                    validate(ParseWholeDocument(record));
                }
        }
        if (!input.eof() && input.fail())
            throw std::runtime_error("A migration document could not be read completely.");
    }
}
std::string ArtifactDigest(const std::filesystem::path& path, const std::filesystem::path& relative)
{
    if (IsDatabase(path))
    {
        auto db = OpenDatabase(path, SQLITE_OPEN_READONLY);
        if (relative == "Memory/revia_memory.db")
            (void)Query(db.get(), "SELECT id,category,summary,normalized_summary,source,created_at FROM memories LIMIT 0");
        else if (relative == "Memory/revia_conversations.db")
        {
            (void)Query(db.get(), "SELECT session_id,started_at,ended_at FROM conversation_sessions LIMIT 0");
            (void)Query(db.get(), "SELECT id,session_id,turn_index,role,content,created_at FROM conversation_turns LIMIT 0");
        }
        else if (relative == "Goals/revia_goals.db")
        {
            (void)Query(
                db.get(), "SELECT id,title,status,stop_reason,current_step,budget,spend,scope,created_at,updated_at FROM goals LIMIT 0");
            auto columns = Query(db.get(), "PRAGMA table_info(goals)");
            while (sqlite3_step(columns.get()) == SQLITE_ROW)
                if (Column(columns.get(), 1) == "verification_schema")
                {
                    auto future = Query(db.get(), "SELECT 1 FROM goals WHERE verification_schema > 1 OR verification_schema < 0 LIMIT 1");
                    if (sqlite3_step(future.get()) != SQLITE_DONE)
                        throw std::runtime_error("Migration goal verification schema is unsupported.");
                }
        }
        return DatabaseDigest(db.get());
    }
    ValidateDocument(path, relative);
    return FileDigest(path);
}

void CopyArtifact(const std::filesystem::path& source, const std::filesystem::path& target)
{
    CheckPath(source);
    CheckPath(target);
    std::filesystem::create_directories(target.parent_path());
    auto temporary = target;
    temporary += ".tmp";
    CheckPath(temporary);
    if (IsDatabase(source))
    {
        // An interrupted backup is disposable staging, never an existing mind's database.
        for (const auto* suffix : {"", "-wal", "-shm", "-journal"})
        {
            auto partial = temporary;
            partial += suffix;
            CheckPath(partial);
            if (std::filesystem::exists(partial) && !std::filesystem::is_regular_file(partial))
                throw std::runtime_error("A migration temporary database is not a regular file.");
            std::filesystem::remove(partial);
        }
        auto from = OpenDatabase(source, SQLITE_OPEN_READONLY);
        auto to = OpenDatabase(temporary, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        sqlite3_backup* backup = sqlite3_backup_init(to.get(), "main", from.get(), "main");
        if (!backup)
            throw std::runtime_error("A migration database backup could not begin.");
        const int code = sqlite3_backup_step(backup, -1);
        const int finish = sqlite3_backup_finish(backup);
        if (code != SQLITE_DONE || finish != SQLITE_OK)
            throw std::runtime_error("A migration database backup did not complete.");
        if (sqlite3_exec(to.get(), "PRAGMA journal_mode=DELETE", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("A migration database could not close its journal.");
    }
    else
        std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::rename(temporary, target);
}
}

CompanionPaths::CompanionPaths(std::filesystem::path absoluteInstallRoot, CompanionDescriptor value)
    : installRoot(std::move(absoluteInstallRoot).lexically_normal()), descriptor(std::move(value))
{
    if (!installRoot.is_absolute() || !SafeId(descriptor.id) || (descriptor.legacy && descriptor.id != Legacy.id) ||
        (!descriptor.legacy && descriptor.id == Legacy.id))
        throw std::invalid_argument("Invalid companion identity or install root.");
    CheckPath(installRoot);
    root = descriptor.legacy ? installRoot : installRoot / "RuntimeData/Companions" / descriptor.id / "v1";
    CheckPath(root);
}
std::filesystem::path CompanionPaths::Resolve(const std::filesystem::path& relative) const
{
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw std::invalid_argument("A companion path must be relative.");
    for (const auto& part : relative)
    {
        const auto text = part.string();
        static const std::regex reserved("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$", std::regex::icase);
        if (part == ".." || text.find(':') != std::string::npos || std::regex_match(text, reserved) ||
            (part != "." && !text.empty() && (text.back() == '.' || text.back() == ' ')))
            throw std::invalid_argument("A companion path cannot escape its root or name a device.");
    }
    const auto resolved = (root / relative).lexically_normal();
    if (!Inside(root, resolved))
        throw std::invalid_argument("A companion path cannot escape its root.");
    CheckPath(resolved);
    return resolved;
}

struct CompanionRegistry::Impl
{
    explicit Impl(std::filesystem::path root) : paths(std::move(root), Legacy)
    {
    }
    CompanionPaths paths;
    mutable std::mutex mutex;
    std::vector<CompanionDescriptor> companions{Legacy};
    std::string selected = Legacy.id;
    bool loaded = false;
    bool Save(const std::vector<CompanionDescriptor>& items, const std::string& selection, std::string& error) const
    {
        json list = json::array();
        for (const auto& item : items)
            if (!item.legacy)
                list.push_back({{"id", item.id}, {"displayName", item.displayName}, {"profileId", item.profileId}});
        return AtomicJson(paths.Resolve("RuntimeData/Companions/registry.json"),
            {{"schemaVersion", 1}, {"selectedId", selection}, {"companions", list}}, error);
    }
};
CompanionRegistry::CompanionRegistry(std::filesystem::path root) : impl(std::make_unique<Impl>(std::move(root)))
{
}
CompanionRegistry::~CompanionRegistry() = default;
bool CompanionRegistry::Load(std::string& error)
{
    std::lock_guard lock(impl->mutex);
    error.clear();
    try
    {
        const auto file = impl->paths.Resolve("RuntimeData/Companions/registry.json");
        std::vector<CompanionDescriptor> list{Legacy};
        std::string selected = Legacy.id;
        if (std::filesystem::exists(file))
        {
            std::ifstream input(file);
            const auto data = ParseWholeDocument(input);
            if (data.at("schemaVersion").get<int>() != 1)
                throw std::runtime_error("Companion registry schema is unsupported.");
            if (!data.at("companions").is_array() || data.at("companions").size() > 256)
                throw std::runtime_error("Companion registry is invalid.");
            for (const auto& item : data.at("companions"))
            {
                CompanionDescriptor value{item.at("id").get<std::string>(), item.at("displayName").get<std::string>(),
                    item.at("profileId").get<std::string>(), false};
                CompanionPaths checked(impl->paths.InstallRoot(), value);
                if (value.displayName.empty() || value.displayName.size() > 128 || !SafeProfile(value.profileId) ||
                    std::any_of(list.begin(), list.end(), [&](const auto& entry) { return entry.id == value.id; }))
                    throw std::runtime_error("Companion registry entry is invalid.");
                list.push_back(std::move(value));
            }
            selected = data.at("selectedId").get<std::string>();
            if (std::none_of(list.begin(), list.end(), [&](const auto& value) { return value.id == selected; }))
                throw std::runtime_error("Companion selection is invalid.");
        }
        impl->companions = std::move(list);
        impl->selected = std::move(selected);
        impl->loaded = true;
        return true;
    }
    catch (const json::exception&)
    {
        error = "The companion registry is invalid or unreadable.";
        impl->loaded = false;
        return false;
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
        impl->loaded = false;
        return false;
    }
}
std::vector<CompanionDescriptor> CompanionRegistry::List() const
{
    std::lock_guard lock(impl->mutex);
    return impl->companions;
}
std::optional<CompanionDescriptor> CompanionRegistry::Find(const std::string& id) const
{
    std::lock_guard lock(impl->mutex);
    for (const auto& value : impl->companions)
        if (value.id == id)
            return value;
    return {};
}
std::string CompanionRegistry::SelectedId() const
{
    std::lock_guard lock(impl->mutex);
    return impl->selected;
}
bool CompanionRegistry::Create(std::string name, std::string profile, CompanionDescriptor& result, std::string& error)
{
    std::lock_guard lock(impl->mutex);
    error.clear();
    if (!impl->loaded || name.empty() || name.size() > 128 || !SafeProfile(profile) || impl->companions.size() > 256)
    {
        error = "Companion creation requires a loaded registry, valid name and profile.";
        return false;
    }
    try
    {
        std::array<unsigned char, 16> random{};
        if (BCryptGenRandom(nullptr, random.data(), random.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("A companion ID could not be created.");
        std::ostringstream id;
        id << "c-";
        for (const auto byte : random)
            id << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        CompanionDescriptor value{id.str(), std::move(name), std::move(profile), false};
        const CompanionPaths paths(impl->paths.InstallRoot(), value);
        if (std::filesystem::exists(paths.Root().parent_path()))
            throw std::runtime_error("A companion ID is already occupied.");
        auto list = impl->companions;
        list.push_back(value);
        if (!impl->Save(list, impl->selected, error))
            return false;
        impl->companions = std::move(list);
        result = std::move(value);
        return true;
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
        return false;
    }
}
bool CompanionRegistry::Select(const std::string& id, std::string& error)
{
    std::lock_guard lock(impl->mutex);
    error.clear();
    if (!impl->loaded || std::none_of(impl->companions.begin(), impl->companions.end(), [&](const auto& value) { return value.id == id; }))
    {
        error = "The selected companion does not exist in the loaded registry.";
        return false;
    }
    if (!impl->Save(impl->companions, id, error))
        return false;
    impl->selected = id;
    return true;
}

std::vector<std::filesystem::path> InventoryLegacyCompanion(const CompanionPaths& source)
{
    if (!source.Descriptor().legacy)
        throw std::invalid_argument("Migration source must be the explicit legacy companion.");
    static const std::array<const char*, 26> allowed{"Memory", "Goals", "Config/Profiles", "RuntimeData/Identity",
        "RuntimeData/Preferences", "RuntimeData/Initiative", "RuntimeData/Reminders", "RuntimeData/Improvement",
        "RuntimeData/ComputerExperience", "RuntimeData/Voices", "RuntimeData/Diagrams", "RuntimeData/Images", "RuntimeData/Presence",
        "RuntimeData/Browser", "RuntimeData/Workspace", "RuntimeData/Vision", "RuntimeData/Camera", "RuntimeData/Recognition", "Logs",
        "RuntimeData/Songs", "RuntimeData/SpeechInput", "RuntimeData/Evaluations", "RuntimeData/Agents", "Audit",
        "RuntimeData/Skills", "RuntimeData/Learning"};
    std::vector<std::filesystem::path> result;
    for (const auto* relative : allowed)
    {
        const auto path = source.Resolve(relative);
        if (!std::filesystem::exists(path))
            continue;
        const auto append = [&](const std::filesystem::path& file)
        {
            CheckPath(file);
            const auto extension = file.extension().string();
            if (extension == "-wal" || extension == "-shm" || file.string().ends_with("-wal") || file.string().ends_with("-shm") ||
                extension == ".tmp")
                return;
            if (std::filesystem::is_regular_file(file))
                result.push_back(file.lexically_relative(source.Root()));
            else if (!std::filesystem::is_directory(file))
                throw std::runtime_error("Migration source includes an unsupported artifact.");
        };
        if (std::filesystem::is_directory(path))
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path))
                append(entry.path());
        else
            append(path);
    }
    std::sort(result.begin(), result.end());
    return result;
}

CompanionMigrationResult MigrateLegacyCompanion(const CompanionPaths& source, const CompanionPaths& target, CompanionMigrationHook hook)
{
    CompanionMigrationResult result;
    const auto proceed = [&](CompanionMigrationPhase phase, const std::filesystem::path& relative)
    {
        if (hook && !hook(phase, relative))
        {
            result.interrupted = true;
            throw std::runtime_error("Companion migration was interrupted.");
        }
    };
    try
    {
        if (!source.Descriptor().legacy || target.Descriptor().legacy || source.InstallRoot() != target.InstallRoot())
            throw std::invalid_argument("Migration requires legacy and new companions at the same install root.");
        CheckPath(target.Root());
        if (std::filesystem::exists(target.Root()))
            throw std::runtime_error("Migration destination is already occupied.");
        auto stage = target.Root();
        stage += ".migration";
        CheckPath(stage);
        const auto manifestFile = stage / "migration-manifest.json";
        auto artifacts = InventoryLegacyCompanion(source);
        proceed(CompanionMigrationPhase::Inventory, {});
        json manifest = {{"schemaVersion", 1}, {"source", source.Root().generic_string()}, {"targetId", target.Descriptor().id},
            {"artifacts", json::object()}};
        if (std::filesystem::exists(manifestFile))
        {
            CheckPath(manifestFile);
            std::ifstream previous(manifestFile);
            manifest = ParseWholeDocument(previous);
            if (manifest.at("schemaVersion") != 1 || manifest.at("source") != source.Root().generic_string() ||
                manifest.at("targetId") != target.Descriptor().id || !manifest.at("artifacts").is_object())
                throw std::runtime_error("Migration staging belongs to another source or schema.");
        }
        std::filesystem::create_directories(stage);
        // Validate every source first; a newer or corrupt artifact never becomes an active partial mind.
        for (const auto& relative : artifacts)
        {
            const auto key = relative.generic_string();
            const auto expected = ArtifactDigest(source.Resolve(relative), relative);
            if (manifest["artifacts"].contains(key) && manifest["artifacts"][key] != expected)
                throw std::runtime_error("Migration source changed since staging began.");
            manifest["artifacts"][key] = expected;
        }
        if (manifest["artifacts"].size() != artifacts.size())
            throw std::runtime_error("Migration source inventory changed since staging began.");
        std::string error;
        if (!AtomicJson(manifestFile, manifest, error))
            throw std::runtime_error(error);
        for (const auto& relative : artifacts)
        {
            const auto key = relative.generic_string();
            const auto destination = stage / relative;
            CheckPath(destination);
            bool reusable = false;
            if (std::filesystem::exists(destination))
                reusable = ArtifactDigest(destination, relative) == manifest["artifacts"][key].get<std::string>();
            if (!reusable)
            {
                if (std::filesystem::exists(destination))
                    throw std::runtime_error("A staged migration artifact is corrupt.");
                proceed(CompanionMigrationPhase::Copy, relative);
                CopyArtifact(source.Resolve(relative), destination);
                ++result.copiedArtifacts;
            }
            proceed(CompanionMigrationPhase::Validate, relative);
            if (ArtifactDigest(destination, relative) != manifest["artifacts"][key].get<std::string>() ||
                ArtifactDigest(source.Resolve(relative), relative) != manifest["artifacts"][key].get<std::string>())
                throw std::runtime_error("Migration artifact validation did not match its source.");
        }
        proceed(CompanionMigrationPhase::Publish, {});
        if (InventoryLegacyCompanion(source) != artifacts)
            throw std::runtime_error("Migration source changed before publication.");
        for (const auto& relative : artifacts)
            if (ArtifactDigest(source.Resolve(relative), relative) != manifest["artifacts"][relative.generic_string()].get<std::string>() ||
                ArtifactDigest(stage / relative, relative) != manifest["artifacts"][relative.generic_string()].get<std::string>())
                throw std::runtime_error("Migration content changed before publication.");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(stage))
        {
            CheckPath(entry.path());
            if (entry.is_directory())
                continue;
            const auto relative = entry.path().lexically_relative(stage);
            if (relative == "migration-manifest.json")
                continue;
            if (!entry.is_regular_file() || !manifest["artifacts"].contains(relative.generic_string()))
                throw std::runtime_error("Migration staging contains an unexpected artifact.");
        }
        CheckPath(target.Root());
        CheckPath(stage);
        if (std::filesystem::exists(target.Root()))
            throw std::runtime_error("Migration destination became occupied.");
        std::filesystem::rename(stage, target.Root());
        result.complete = true;
    }
    catch (const json::exception&)
    {
        result.error = "A migration document or manifest is invalid or unsupported.";
    }
    catch (const std::exception& failure)
    {
        result.error = failure.what();
    }
    return result;
}
}
