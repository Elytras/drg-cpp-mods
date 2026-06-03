#pragma once
// CliTypes.h — Shared types, constants, extern global declarations, and inline helpers.
// Include this at the top of any CLI implementation header.

#include <Windows.h>
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "../SharedLib/IpcProtocol.h"
#include "Profile.h"
#include "SplitConsole.h"
#include "WinInput.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Types
// ─────────────────────────────────────────────────────────────────────────────

struct KnownFunction
{
    std::string name;
    std::string params;
    std::string owner;
};

struct KnownCommand
{
    std::string name;
    std::string desc;
    std::string params;
};

enum class InjectionState { Watching, Active, Suppressed };

// ─────────────────────────────────────────────────────────────────────────────
//  Compile-time constants
// ─────────────────────────────────────────────────────────────────────────────

extern DWORD   kWatchPollMs;
constexpr wchar_t kDumper7Path[] = LR"(D:\Repos\Dumper7\x64\Release\Dumper-7.dll)";

inline const std::vector<std::string> k_BuiltinCmds = {
    "load", "unload", "reload", "launch", "d7l", "d7u", "exit", "killgame", "mode"
};

// ─────────────────────────────────────────────────────────────────────────────
//  Globals  (defined in DrgCli.cpp)
// ─────────────────────────────────────────────────────────────────────────────

extern std::atomic<bool>           g_Running;
extern std::atomic<bool>           g_Dumper7Loaded;
extern std::atomic<InjectionState> g_InjState;
extern HANDLE                      g_hProcess;
extern std::mutex                  g_InjectionMutex;

extern HANDLE g_hLogEvent;
extern HANDLE g_hCmdEvent;
extern HANDLE g_hShutdownEvent;
extern HANDLE g_hRespEvent;
extern HANDLE g_hDllReadyEvent;
extern HANDLE g_hLogMapping;
extern HANDLE g_hCmdMapping;
extern HANDLE g_hRespMapping;
extern HANDLE g_hMetaMapping;

// Serializes CLI-side send+wait cycles to the DLL so the REPL and
// auto-listcmds thread don't clobber each other's responses.
extern std::mutex g_DllCommMutex;

extern LogBuffer*      g_pLogBuffer;
extern CommandBuffer*  g_pCmdBuffer;
extern ResponseBuffer* g_pRespBuffer;
extern MetaBuffer*     g_pMetaBuffer;

extern std::wstring g_SourceDllPath;
extern std::wstring g_CopyDllPath;
extern FILETIME     g_LastInjectedTime;

extern SplitConsole*              g_pSplit;
extern std::vector<KnownFunction> g_KnownFunctions;
extern std::vector<KnownCommand>  g_KnownCommands;

// ─────────────────────────────────────────────────────────────────────────────
//  Inline helpers
// ─────────────────────────────────────────────────────────────────────────────

inline bool DllActive()     { return g_InjState.load() == InjectionState::Active; }
inline bool DllSuppressed() { return g_InjState.load() == InjectionState::Suppressed; }

inline void Log(const std::string& msg)
{
    if (g_pSplit)
        g_pSplit->PrintLog("[INJ] " + msg);
    else
        std::cout << "[INJ] " << msg << "\n";
}
