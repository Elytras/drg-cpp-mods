#pragma once
// SharedCommands.h — game-agnostic commands shared by DrgMods and RcMods.
//
// Compiled once per consumer (DrgMods / RcMods) against that game's Library.h,
// exactly like ModManager.cpp and the other SharedLib *.cpp (each is listed in
// SharedLib.vcxproj as ExcludedFromBuild and pulled into each DLL project via
// $(SolutionDir)SharedLib\…). RegisterSharedCommands() registers every command
// whose implementation depends only on SDK symbols common to both games' SDKs.
#include "Lib_CommandHandler.h"

// Register all shared commands into the handler.
// Call from each game's RegisterCommands() before its game-specific registrations.
void RegisterSharedCommands(CommandHandler& handler);

// Install always-on shared game-thread callbacks: the world-change watcher that
// drives each game's OnWorldChanged() (cache invalidation) and flushes netfreq.
// Call once from each game's InitDefaultCallbacks() (game thread). Idempotent.
void InitSharedCallbacks();
