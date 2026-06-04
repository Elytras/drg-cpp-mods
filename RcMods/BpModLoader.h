#pragma once
#include <cstdint>
#include <functional>
// BpModLoader.h — Asset-registry-driven Blueprint mod loader.
//
// Watches for UWorld transitions on the EngineTick (After) and spawns one
// instance of every Blueprint class found under /Game/_Mods/ whose short name
// begins with "InitMod_" and whose generated class is an AActor subclass.
//
// World-change callbacks fire after each spawn pass completes, letting other
// systems react to level transitions without needing their own tick hook.
//
// Usage:
//   BpModLoader::Install()   — call from LoadModsGameThread(), registers the
//                              engine-tick watcher.
//   BpModLoader::Uninstall() — call from UnloadMods(), removes the watcher
//                              and clears all OnWorldChanged subscriptions.
//
//   auto h = BpModLoader::OnWorldChanged([](SDK::UWorld* w) { ... });
//   BpModLoader::RemoveOnWorldChanged(h);  // unsubscribe

namespace SDK { class UWorld; }

namespace BpModLoader
{
    using WorldChangedFn      = std::function<void(SDK::UWorld*)>;
    using WorldCallbackHandle = uint32_t;

    // Register a callback invoked (on the game thread) whenever the active
    // UWorld changes and InitMod_ actors have been spawned.
    // Returns a handle that can be passed to RemoveOnWorldChanged.
    WorldCallbackHandle OnWorldChanged(WorldChangedFn callback);

    // Unregister a previously registered callback.  No-op for invalid handles.
    void RemoveOnWorldChanged(WorldCallbackHandle handle);

    void Install();
    void Uninstall();
}
