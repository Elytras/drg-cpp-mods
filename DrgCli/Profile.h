#pragma once
// Profile.h — Runtime-selectable per-game string constants for the CLI.
//
// The CLI is a single binary that can talk to either DrgMods.dll or RcMods.dll.
// Selection happens at startup via argv (or auto-detection). All injector /
// shared-memory code reads from the global `g_Profile`, never from a hardcoded
// constant.
//
// Per-game DLLs still hardcode their own constants in DrgMods/Common.h and
// RcMods/Common.h respectively — the Profile values here must stay in lock-
// step with whichever DLL the user is targeting.

#include <Windows.h>
#include <cstdint>

enum class GameId : uint8_t
{
    DRG = 0,  // Deep Rock Galactic
    RC  = 1,  // RogueCore
};

struct Profile
{
    GameId        id;
    const wchar_t* name;             // "DRG" / "RC" — used in log prefixes

    // Process / DLL identity
    const wchar_t* targetProcess;        // e.g. "FSD-Win64-Shipping.exe"
    const wchar_t* dllFilename;          // e.g. "DrgMods.dll"
    const char*    targetProcessNarrow;  // narrow copy of targetProcess

    // Direct-launch support (CLI `launch`). Full path to the game exe + extra args.
    // Working dir is derived from exePath so the game's steam_appid.txt (placed next
    // to the exe) is read and Steam initialises on a direct launch.
    const wchar_t* exePath;              // full path to the game exe ("" if unset)
    const wchar_t* launchArgs;           // extra CLI args, e.g. "-disablemodding"

    // Shared memory names
    const wchar_t* shmemLogs;
    const wchar_t* shmemInjLog;
    const wchar_t* shmemCmd;
    const wchar_t* shmemResponse;
    const wchar_t* shmemMeta;

    // Event names
    const wchar_t* eventLogReady;
    const wchar_t* eventInjLogReady;
    const wchar_t* eventCmdReady;
    const wchar_t* eventRespReady;
    const wchar_t* eventShutdown;
    const wchar_t* eventShutdownDone;
    const wchar_t* eventDllReady;
};

// Profile bank — one per supported game.
extern const Profile kDrgProfile;
extern const Profile kRcProfile;

// Currently active profile — set once at startup, then read everywhere.
extern const Profile* g_Profile;

// Resolve a profile from a command-line token ("drg", "rc", case-insensitive).
// Returns nullptr on unknown token.
const Profile* ResolveProfile(const wchar_t* token);

// Initialize g_Profile. If `requested` is null, auto-detect by scanning for a
// running game process; if neither runs, defaults to DRG. Returns the chosen
// profile.
const Profile* InitProfile(const Profile* requested);
