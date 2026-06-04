#pragma once
// Commands.h — Command registration and lifecycle functions for ModManager (RogueCore).

#include "Lib_CommandHandler.h"

// Register all game commands into the handler — call once in ModManager constructor
void RegisterCommands(CommandHandler& handler);

// Register default-on callbacks — call in LoadModsGameThread
void InitDefaultCallbacks();

// Zero all callback handles — call in UnloadMods before RequestUninstall
void ResetCallbackHandles();

// ── Game policy hooks — called by the shared ModManager ──────────────────────
// PreLoadCheck:    worker thread, before any hook install; return false to abort.
// OnModsLoaded:    game thread, after hooks installed, before InitDefaultCallbacks.
// OnModsUnloading: start of UnloadMods, before hook teardown.
bool PreLoadCheck();
void OnModsLoaded();
void OnModsUnloading();

// OnWorldChanged: game thread, called by the shared world-watcher whenever the
// active UWorld pointer changes. Invalidate world-scoped caches here.
void OnWorldChanged();

// Send the list of registered commands and their descriptions back to the CLI for help display and autocompletion.
void SendCommandList(const CommandContext& ctx, const CommandHandler& handler);

// Check TickActor validity and restart search if needed — call periodically from ModManager::UpdateGameThread
namespace TickSystem
{
    void Dispatch(long double actualDeltaMs);
    void Reset();
}
