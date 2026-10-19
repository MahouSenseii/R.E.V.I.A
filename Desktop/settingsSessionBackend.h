#pragma once

#include "settingsTypes.h"

#include <functional>

namespace revia::runtime
{
class ReviaSession;
}

namespace revia::desktop
{
// What the session adapter needs from the window.
struct SessionBackendDeps
{
    // The session the dialog is showing. Resolved on the UI thread when a read or write starts.
    std::function<revia::runtime::ReviaSession&()> session;
    // Runs one write off the UI thread. The window's serial settings worker, so writes keep
    // their order and are joined before the session they target retires.
    std::function<void(std::function<void()>)> runInBackground;
    // Queues a callback onto the UI thread; the window drops it if the session changed meanwhile.
    std::function<void(std::function<void()>)> postToUi;
    // True while the Profiles editor holds unsaved edits, which a profile switch would overwrite.
    std::function<bool()> profileDraftDirty;
    // Called on the UI thread after a profile switch was saved, so the Profiles editor re-reads it.
    std::function<void()> afterProfileWrite;
};

// The Settings dialog's backend over a ReviaSession, dispatching on SettingKeyRef::store:
// - Preference: SetPreference / ResetPreference.
// - SettingsFile: SetOwnerSetting / SetOwnerChoice / ResetOwnerSetting.
// - Capability: the guest switches (keys ending in .enabled, .read, .write, .execute,
//   .allowTaskExecution), through SetGuestCapabilities with one flag changed.
// - Session: "perception.paused" and "cloud.session".
// - VirtualMachine: any key; Set/IsVirtualMachineEnabled.
// - Profile: "profile.memoryEnabled", "profile.hasTemperatureOverride" and
//   "profile.hasMaxTokensOverride" on the active profile, refused while the Profiles editor
//   has unsaved edits.
// readAll, readChoice, readProfileValue, mcpServers and statusText run on the UI thread and
// return at once. Every write runs through runInBackground and calls done on the UI thread
// through postToUi, including a write refused before it starts.
SettingsBackend MakeSessionSettingsBackend(SessionBackendDeps deps);
}
