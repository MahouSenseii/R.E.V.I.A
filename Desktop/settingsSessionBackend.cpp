#include "settingsSessionBackend.h"

#include "Runtime/reviaSession.h"

#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace revia::desktop
{
namespace
{
using revia::runtime::ReviaSession;

constexpr auto NotStarted = "Available once Revia has started.";
constexpr auto UnknownSetting = "Revia does not know this setting.";
constexpr auto ProfileDraftOpen = "Save or discard your changes in Profiles first.";

struct Context
{
    SessionBackendDeps deps;
};

SettingWrite Result(const bool succeeded, const std::string& message)
{
    return {succeeded, QString::fromStdString(message)};
}

constexpr std::string_view GuestPrefix = "capabilities.guest.";

// The Ubuntu workspace switch a capability key names (capabilities.guest.execute -> execute),
// or null. Only the guest's own keys: every other capability ends in .enabled too, and must
// never reach a guest permission.
bool* GuestFlag(revia::policy::GuestCapabilitySettings& guest, const std::string& key)
{
    if (!key.starts_with(GuestPrefix))
        return nullptr;
    const std::string field = key.substr(GuestPrefix.size());
    if (field == "enabled")
        return &guest.enabled;
    if (field == "read")
        return &guest.read;
    if (field == "write")
        return &guest.write;
    if (field == "execute")
        return &guest.execute;
    if (field == "allowTaskExecution")
        return &guest.allowTaskExecution;
    return nullptr;
}

bool IsGuestKey(const std::string& key)
{
    revia::policy::GuestCapabilitySettings probe;
    return GuestFlag(probe, key) != nullptr;
}

bool* ProfileFlag(revia::runtime::ProfileSummary& profile, const std::string& key)
{
    if (key == "profile.memoryEnabled")
        return &profile.memoryEnabled;
    if (key == "profile.hasTemperatureOverride")
        return &profile.hasTemperatureOverride;
    if (key == "profile.hasMaxTokensOverride")
        return &profile.hasMaxTokensOverride;
    return nullptr;
}

SettingRead FromState(const revia::runtime::BooleanSettingState& state)
{
    return {state.available, state.value, state.running, QString::fromStdString(state.unavailableReason)};
}

SettingRead Unavailable(const char* reason)
{
    SettingRead read;
    read.unavailableReason = QString::fromUtf8(reason);
    return read;
}

// Runs work on the settings worker and hands its result to done on the UI thread. Nothing
// thrown may leave the worker, so an exception becomes a failed write.
void Dispatch(const std::shared_ptr<Context>& context, std::function<SettingWrite()> work, SettingDone done,
    std::function<void()> afterSuccess = {})
{
    context->deps.runInBackground(
        [context, work = std::move(work), done = std::move(done), afterSuccess = std::move(afterSuccess)]()
        {
            SettingWrite result;
            try
            {
                result = work();
            }
            catch (const std::exception& error)
            {
                result = {false, QString::fromUtf8(error.what())};
            }
            context->deps.postToUi(
                [result, done, afterSuccess]()
                {
                    if (result.succeeded && afterSuccess)
                        afterSuccess();
                    if (done)
                        done(result);
                });
        });
}

// A write refused before it starts still answers on the UI thread, after the caller returns.
void Refuse(const std::shared_ptr<Context>& context, SettingDone done, const char* reason)
{
    context->deps.postToUi(
        [done = std::move(done), reason]()
        {
            if (done)
                done({false, QString::fromUtf8(reason)});
        });
}

std::function<SettingWrite()> ProfileWork(ReviaSession* session, std::function<bool(revia::runtime::ProfileSummary&)> change)
{
    return [session, change = std::move(change)]()
    {
        std::optional<revia::runtime::ProfileSummary> active = session->ActiveProfileSummary();
        if (!active)
            return Result(false, NotStarted);
        if (!change(*active))
            return Result(false, UnknownSetting);
        // Refused at once during a turn rather than waiting for it: writes share one queue, and
        // a profile switch must not hold up a privacy switch behind a long reply.
        const revia::runtime::ProfileOperationResult saved = session->TrySaveProfile(*active);
        return Result(saved.succeeded, saved.message);
    };
}
} // namespace

SettingsBackend MakeSessionSettingsBackend(SessionBackendDeps deps)
{
    const auto context = std::make_shared<Context>(Context{std::move(deps)});
    SettingsBackend backend;

    backend.readAll = [context](const std::vector<SettingKeyRef>& refs)
    {
        ReviaSession& session = context->deps.session();
        std::map<std::string, SettingRead> reads;
        std::optional<std::map<std::string, revia::runtime::BooleanSettingState>> booleans;
        std::optional<revia::policy::GuestCapabilitySettings> guest;
        std::optional<std::optional<revia::runtime::ProfileSummary>> profile;
        for (const SettingKeyRef& ref : refs)
        {
            SettingRead read = Unavailable(UnknownSetting);
            switch (ref.store)
            {
            case SettingStore::Preference:
            case SettingStore::SettingsFile:
            case SettingStore::Session:
            case SettingStore::VirtualMachine:
            {
                if (!booleans)
                    booleans = session.BooleanSettings();
                const auto found = booleans->find(ref.store == SettingStore::VirtualMachine ? "virtualMachine.enabled" : ref.key);
                if (found != booleans->end())
                    read = FromState(found->second);
                break;
            }
            case SettingStore::Capability:
            {
                if (!IsGuestKey(ref.key))
                    break;
                if (!session.IsStarted())
                {
                    read = Unavailable(NotStarted);
                    break;
                }
                if (!guest)
                    guest = session.Capabilities().guest;
                const bool flag = *GuestFlag(*guest, ref.key);
                read = {true, flag, flag, {}};
                break;
            }
            case SettingStore::Profile:
            {
                if (!profile)
                    profile = session.ActiveProfileSummary();
                if (!*profile)
                {
                    read = Unavailable(NotStarted);
                    break;
                }
                if (const bool* flag = ProfileFlag(**profile, ref.key))
                    read = {true, *flag, *flag, {}};
                break;
            }
            case SettingStore::Shell:
                break;
            }
            reads[ref.key] = read;
        }
        return reads;
    };

    backend.write = [context](const SettingKeyRef& ref, const bool value, SettingDone done)
    {
        ReviaSession* session = &context->deps.session();
        const std::string key = ref.key;
        switch (ref.store)
        {
        case SettingStore::Preference:
            Dispatch(context,
                [session, key, value]()
                {
                    const auto result = session->SetPreference(key, value ? "on" : "off");
                    return Result(result.succeeded, result.message);
                },
                std::move(done));
            return;
        case SettingStore::SettingsFile:
            Dispatch(context,
                [session, key, value]()
                {
                    const auto result = session->SetOwnerSetting(key, value);
                    return Result(result.succeeded, result.message);
                },
                std::move(done));
            return;
        case SettingStore::Capability:
        {
            if (!IsGuestKey(key))
                break;
            Dispatch(context,
                [session, key, value]()
                {
                    revia::policy::GuestCapabilitySettings guest = session->Capabilities().guest;
                    *GuestFlag(guest, key) = value;
                    const auto result = session->SetGuestCapabilities(guest);
                    if (!result.succeeded)
                        return Result(false, result.message);
                    // The editor drops a switch whose parent permission is off.
                    revia::policy::GuestCapabilitySettings saved = session->Capabilities().guest;
                    if (*GuestFlag(saved, key) != value)
                        return Result(false, "This permission needs the one it belongs to turned on first.");
                    return Result(true, result.message);
                },
                std::move(done));
            return;
        }
        case SettingStore::Session:
            Dispatch(context,
                [session, key, value]()
                {
                    if (key == "perception.paused")
                    {
                        if (!session->IsStarted() || !session->IsPerceptionEnabled())
                            return Result(false, "App awareness is not running.");
                        session->SetPerceptionPaused(value);
                        return Result(true, value ? "App awareness is paused." : "App awareness is back on.");
                    }
                    if (key == "cloud.session")
                    {
                        session->SetCloudSessionEnabled(value);
                        return Result(true, value ? "The cloud model is allowed again for this session."
                                                  : "The cloud model is off for this session; every turn stays on this PC.");
                    }
                    return Result(false, UnknownSetting);
                },
                std::move(done));
            return;
        case SettingStore::VirtualMachine:
            Dispatch(context,
                [session, value]()
                {
                    const auto result = session->SetVirtualMachineEnabled(value);
                    return Result(result.succeeded, result.message);
                },
                std::move(done));
            return;
        case SettingStore::Profile:
            if (context->deps.profileDraftDirty && context->deps.profileDraftDirty())
            {
                Refuse(context, std::move(done), ProfileDraftOpen);
                return;
            }
            Dispatch(context,
                ProfileWork(session,
                    [key, value](revia::runtime::ProfileSummary& active)
                    {
                        bool* flag = ProfileFlag(active, key);
                        if (flag)
                            *flag = value;
                        return flag != nullptr;
                    }),
                std::move(done), context->deps.afterProfileWrite);
            return;
        case SettingStore::Shell:
            break;
        }
        Refuse(context, std::move(done), UnknownSetting);
    };

    backend.reset = [context](const SettingKeyRef& ref, SettingDone done)
    {
        ReviaSession* session = &context->deps.session();
        const std::string key = ref.key;
        if (ref.store == SettingStore::Preference)
        {
            Dispatch(context,
                [session, key]()
                {
                    const auto result = session->ResetPreference(key);
                    return Result(result.succeeded, result.message);
                },
                std::move(done));
            return;
        }
        if (ref.store == SettingStore::SettingsFile)
        {
            Dispatch(context,
                [session, key]()
                {
                    const auto result = session->ResetOwnerSetting(key);
                    return Result(result.succeeded, result.message);
                },
                std::move(done));
            return;
        }
        Refuse(context, std::move(done), "This setting has no saved default to return to.");
    };

    backend.readChoice = [context](const std::string& key)
    {
        // The dialog names the active profile in its Chat group; that name is the profile's own,
        // not an owner choice (empty before the session has a profile).
        if (key == "profile.displayName")
        {
            const std::optional<revia::runtime::ProfileSummary> active = context->deps.session().ActiveProfileSummary();
            if (!active)
                return QString();
            return QString::fromStdString(active->displayName.empty() ? active->id : active->displayName);
        }
        return QString::fromStdString(context->deps.session().OwnerChoice(key));
    };

    backend.writeChoice = [context](const std::string& key, const QString& value, SettingDone done)
    {
        ReviaSession* session = &context->deps.session();
        Dispatch(context,
            [session, key, choice = value.toStdString()]()
            {
                const auto result = session->SetOwnerChoice(key, choice);
                return Result(result.succeeded, result.message);
            },
            std::move(done));
    };

    backend.readProfileValue = [context](const std::string& field)
    {
        const std::optional<revia::runtime::ProfileSummary> active = context->deps.session().ActiveProfileSummary();
        if (!active)
            return 0.0;
        if (field == "temperature")
            return static_cast<double>(active->temperature);
        if (field == "maxTokens")
            return static_cast<double>(active->maxTokens);
        return 0.0;
    };

    backend.writeProfileValue = [context](const std::string& field, const double value, SettingDone done)
    {
        if (context->deps.profileDraftDirty && context->deps.profileDraftDirty())
        {
            Refuse(context, std::move(done), ProfileDraftOpen);
            return;
        }
        Dispatch(context,
            ProfileWork(&context->deps.session(),
                [field, value](revia::runtime::ProfileSummary& active)
                {
                    if (field == "temperature")
                        active.temperature = static_cast<float>(value);
                    else if (field == "maxTokens")
                        active.maxTokens = static_cast<int>(std::lround(value));
                    else
                        return false;
                    return true;
                }),
            std::move(done), context->deps.afterProfileWrite);
    };

    backend.mcpServers = [context]() { return context->deps.session().McpServerNames(); };

    backend.statusText = [context](const std::string& key)
    {
        ReviaSession& session = context->deps.session();
        if (key == "llm.cloud.enabled")
            return QString::fromStdString(session.CloudStatusText());
        if (key == "speechRecognition.requireWakeWord")
        {
            const std::vector<std::string> words = session.WakeWords();
            if (words.empty())
                return QStringLiteral("No wake words are set in Config/settings.json, so this cannot be turned on.");
            QStringList shown;
            for (std::size_t index = 0; index < words.size() && index < 3; ++index)
                shown << QString::fromStdString(words[index]);
            return QStringLiteral("Answers only when it hears: ") + shown.join(QStringLiteral(", ")) +
                   (words.size() > 3 ? QString(QChar(0x2026)) : QString());
        }
        return QString();
    };

    return backend;
}
}
