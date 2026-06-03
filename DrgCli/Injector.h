#pragma once
// Injector.h — DLL injection, shared memory, Dumper7, and background watcher threads.

#include <Windows.h>
#include <TlHelp32.h>
#include <filesystem>

#include "CliTypes.h"

// ── Process helpers ───────────────────────────────────────────────────────────
DWORD_PTR FindRemoteModuleByName(DWORD pid, const std::wstring& moduleName);
DWORD     GetProcId(const wchar_t* procName);
bool      IsProcessAlive(HANDLE hProcess);

// ── DLL path helpers ──────────────────────────────────────────────────────────
bool SourceDllUpdated();
bool ResolveDllPaths();

// ── Shared memory ─────────────────────────────────────────────────────────────
bool InitSharedMemory();
void CleanupSharedMemory();

// ── Inject / Unload ───────────────────────────────────────────────────────────
bool InjectDLL();
void UnloadDLL(bool suppress = false);

// ── Launch ────────────────────────────────────────────────────────────────────
// Start the active profile's game exe (g_Profile->exePath) with its launchArgs,
// working-dir = the exe's folder (so steam_appid.txt next to it is read). The
// process watcher then auto-injects. No-op if already running or no path set.
bool LaunchGame();

// ── Dumper7 ───────────────────────────────────────────────────────────────────
bool LoadDumper7();
bool UnloadDumper7();

// ── Background threads ────────────────────────────────────────────────────────
void DllLogThread(LogBuffer* pLog, HANDLE hLogEvent);
void HotReloadThread();
void ProcessWatcherThread();

#ifdef _DEBUG
void AttachDebugger(DWORD pid, DWORD timeoutMs = 15000);
#endif
