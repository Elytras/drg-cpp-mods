// DrgCli.cpp — Combined CLI + injector host.
//   • Creates all shared memory (log, cmd, response) and manages DLL lifecycle.
//   • Watches for the target process and auto-injects DrgMods.dll.
//   • Runs a readline REPL with tab-completion, ghost hints, and AC pane.
//
// Replaces the former separate DrgInjector (ConsoleHost) and DrgCli executables.


#define _CRT_SECURE_NO_WARNINGS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define VERBOSE 0
#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <string>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_set>
#include <iostream>

#ifdef TESTING
#include "../DrgMods/SDK/SDK/CoreUObject_classes.hpp"
#include "../DrgMods/SDK/UnrealContainers.hpp"
#else
#endif

#include "../DrgMods/Common.h"
#include "../DrgMods/StringLib.h"

#include "SplitConsole.h"
#include "WinInput.h"
#include "Injector.h"
#include "Completion.h"

// ─────────────────────────────────────────────────────────────────────────────
//  CLI types
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
//  Injection state machine
//   Watching   – auto-inject whenever the target process appears
//   Active     – DLL is currently loaded inside the target
//   Suppressed – "unload" was issued; watcher stays hands-off until "load"
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool>           g_Running{ true };
static std::atomic<bool>           g_Dumper7Loaded{ false };
static std::atomic<InjectionState> g_InjState{ InjectionState::Watching };
static HANDLE                      g_hProcess = NULL;
static std::mutex                  g_InjectionMutex;

static DWORD                       kWatchPollMs = 1000;

// Shared memory handles
static HANDLE g_hLogEvent      = NULL;
static HANDLE g_hCmdEvent      = NULL;
static HANDLE g_hShutdownEvent = NULL;
static HANDLE g_hRespEvent     = NULL;
static HANDLE g_hLogMapping    = NULL;
static HANDLE g_hCmdMapping    = NULL;
static HANDLE g_hRespMapping   = NULL;

static LogBuffer*      g_pLogBuffer  = nullptr;
static CommandBuffer*  g_pCmdBuffer  = nullptr;
static ResponseBuffer* g_pRespBuffer = nullptr;

// DLL paths
static std::wstring g_SourceDllPath;
static std::wstring g_CopyDllPath;
static FILETIME     g_LastInjectedTime{};

// CLI state
static SplitConsole*              g_pSplit = nullptr;
static std::vector<KnownFunction> g_KnownFunctions;
static std::vector<KnownCommand>  g_KnownCommands;

// ─────────────────────────────────────────────────────────────────────────────
//  Command dispatch
//  Returns 0 if the command was handled internally (no DLL response to wait for).
//  Returns >0 (the seq number) if the command was forwarded to the DLL.
// ─────────────────────────────────────────────────────────────────────────────

static uint32_t DispatchCommand(const std::string& cmd)
{
    if (cmd == "load")
    {
        if (DllActive())
            Log("DLL already loaded.");
        else
        {
            g_InjState.store(InjectionState::Watching);
            InjectDLL();
        }
        return 0;
    }
    if (cmd == "unload")
    {
        if (DllActive())
            UnloadDLL(true);
        else
        {
            g_InjState.store(InjectionState::Suppressed);
            Log("DLL not loaded; auto-injection suppressed until next 'load'.");
        }
        return 0;
    }
    if (cmd == "reload") { UnloadDLL(false); Sleep(500); InjectDLL(); return 0; }
    if (cmd == "d7l")    { LoadDumper7();   return 0; }
    if (cmd == "d7u")    { UnloadDumper7(); return 0; }
    if (cmd == "killgame")
    {
        if (!g_hProcess) { Log("Game process not found."); return 0; }
        Log("Killing game process.");
        TerminateProcess(g_hProcess, 0);
        return 0;
    }

    // Forward to DLL
    if (!DllActive())
    {
        Log("Cannot forward command — DLL not loaded.");
        return 0;
    }
    while (g_pCmdBuffer->hasCommand.load(std::memory_order_acquire))
        Sleep(10);
    uint32_t seq = g_pCmdBuffer->seq.fetch_add(1, std::memory_order_relaxed) + 1;
    strncpy_s(g_pCmdBuffer->command, cmd.c_str(), sizeof(g_pCmdBuffer->command) - 1);
    g_pCmdBuffer->hasCommand.store(true, std::memory_order_release);
    SetEvent(g_hCmdEvent);
    return seq;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Response handling
// ─────────────────────────────────────────────────────────────────────────────

struct PendingOutput
{
    std::string text;
    bool        isScan = false;
};

static PendingOutput ConsumeResponse(ResponseBuffer* pResp, HANDLE hRespEvent)
{
    PendingOutput out{};

    if (pResp->type == ResponseType::Scan)
    {
        const auto& sr = pResp->data.scan;
        g_KnownFunctions.clear();
        for (uint32_t i = 0; i < sr.count; ++i)
            g_KnownFunctions.push_back({ sr.funcs[i].name, sr.funcs[i].params, sr.funcs[i].owner });
        out.isScan = true;
        out.text = "Scanned " + std::to_string(sr.count) + " functions.";
    }
    else if (pResp->type == ResponseType::Commands)
    {
        const auto& cr = pResp->data.commands;
        g_KnownCommands.clear();
        for (uint32_t i = 0; i < cr.count; ++i)
        {
            KnownCommand kc;
            kc.name = cr.cmds[i].name;
            kc.desc = cr.cmds[i].desc;
            size_t sp = kc.desc.find(' ');
            kc.params = (sp != std::string::npos) ? kc.desc.substr(sp + 1) : "";
            g_KnownCommands.push_back(std::move(kc));
        }
        out.text = "Loaded " + std::to_string(cr.count) + " commands.";
    }
    else
    {
        const char* text = pResp->data.text;
        if (text[0] != '\0' && !(text[0] == 'o' && text[1] == 'k' && text[2] == '\0'))
            out.text = text;
    }

    pResp->ready.store(false, std::memory_order_release);
    ResetEvent(hRespEvent);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Ctrl handler
// ─────────────────────────────────────────────────────────────────────────────

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType)
    {
    case CTRL_CLOSE_EVENT:
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        g_Running.store(false, std::memory_order_relaxed);
        return FALSE;
    default:
        return FALSE;
    }
}
struct FNetworkGUID {
    uint32_t Value;
};

class UPackageMap : public SDK::UObject {
    bool					    bSuppressLogs;

    bool					    bShouldTrackUnmappedGuids;
    UC::TSet< FNetworkGUID >	TrackedUnmappedNetGuids;
    UC::TSet< FNetworkGUID >	TrackedMappedDynamicNetGuids;

    UC::FString					DebugContextString;
};

class FLinkerInstancingContext {
    UC::TMap<SDK::FName, SDK::FName> Mapping;
};
// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    system("chcp 65001 > nul");

    // ── Split console ──────────────────────────────────────────────────────
    SplitConsole split;
    g_pSplit = &split;

    auto Print = [&](const std::string& s) { split.PrintCmd(s); };
    Print("--- DRG Mod CLI ---  Tab: complete  Right: accept hint  Up/Down: history");
#if TESTING 
    Print(
        std::to_string(sizeof(UC::FString)) + " " +
        std::to_string(offsetof(UC::FString, Data)) + " " +
        std::to_string(offsetof(UC::FString, NumElements)) + " " +
        std::to_string(offsetof(UC::FString, MaxElements)) + " " +
        std::to_string(sizeof(UPackageMap)) + " " + 
        std::to_string(sizeof(UPackageMap) - sizeof(SDK::UObject)) + " " +
        std::to_string(sizeof(FLinkerInstancingContext)));
#endif
    // ── Shared memory ──────────────────────────────────────────────────────
    if (!InitSharedMemory())
    {
        Print("Failed to initialize shared memory. Exiting.");
        Sleep(2000);
        return 1;
    }

    // ── Background threads ─────────────────────────────────────────────────
    std::thread logThread(DllLogThread, g_pLogBuffer, g_hLogEvent);
    std::thread hotThread(HotReloadThread);
    std::thread watchThread(ProcessWatcherThread);

    Log("Ready. Watching for " + std::string(TARGET_PROCESS_NARROW) + "...");

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // ── WinInput setup ─────────────────────────────────────────────────────
    WinInput input;
    input.SetCompletionCallback(CompletionCallback);
    input.SetHintCallback(HintCallback);
    input.SetHistoryMaxLen(200);

    input.SetSplitMode(
        &split.Mutex(),
        [&](const std::string& s) { split.PrintCmdUnderLock(s); },
        [&]() -> SHORT { split.HandleResizeLocked(); return split.InputRow(); },
        [&]() -> SHORT { return split.InputRow(); }
    );
    input.SetScrollFn([&](int d) { split.ScrollLogUnderLock(d); });
    input.SetAcMode(
        [&](int acH) -> SHORT { return split.SetAcHeightUnderLock(acH); },
        [&]()                 { split.ClearAcUnderLock(); }
    );

    input.SetDescriptionCallback([](const std::string& candidate) -> std::string {
        if (IEquals(candidate, "load"))   return "Load the DLL into the game process";
        if (IEquals(candidate, "unload")) return "Unload the DLL from the game process";
        if (IEquals(candidate, "reload")) return "Hot-reload the DLL";
        if (IEquals(candidate, "d7l"))    return "Load Dumper7 into the game process";
        if (IEquals(candidate, "d7u"))    return "Unload Dumper7 (in-game F6 only)";
        if (IEquals(candidate, "exit"))     return "Exit the CLI";
        if (IEquals(candidate, "killgame")) return "Kill the game process immediately";

        for (const auto& kc : g_KnownCommands)
            if (IEquals(kc.name, candidate)) return kc.desc;

        if (candidate.size() > 5 && candidate.substr(0, 5) == "call ")
        {
            std::string fn = candidate.substr(5);
            auto dpos = fn.find(" :: ");
            if (dpos != std::string::npos) fn = fn.substr(0, dpos);
            for (const auto& kf : g_KnownFunctions)
                if (IEquals(kf.name, fn))
                    return (kf.params.empty() ? "" : "(" + kf.params + ")  ") + "[" + kf.owner + "]";
        }
        return "";
    });

    // ── REPL ───────────────────────────────────────────────────────────────
    while (true)
    {
        std::string line;
        if (!input.Readline("> ", line))
            break;

        if (line.empty()) continue;

        input.HistoryAdd(line);

        if (_strcmpi(line.c_str(), "exit") == 0)
        {
            UnloadDLL(false);
            break;
        }

        uint32_t mySeq = DispatchCommand(line);

        if (mySeq == 0) continue; // built-in or "DLL not loaded" — handled internally

        if (!g_pRespBuffer || !g_hRespEvent)
        {
            Print("Sent (no response buffer).");
            continue;
        }

        constexpr DWORD kTimeoutMs = 5000;
        DWORD64 deadline = GetTickCount64() + kTimeoutMs;
        PendingOutput pending{};
        bool gotResponse = false;

        while (GetTickCount64() < deadline)
        {
            if (g_pRespBuffer->ready.load(std::memory_order_acquire) &&
                g_pRespBuffer->seq.load(std::memory_order_acquire) == mySeq)
            {
                pending = ConsumeResponse(g_pRespBuffer, g_hRespEvent);
                gotResponse = true;
                break;
            }

            DWORD64 remaining = deadline - GetTickCount64();
            WaitForSingleObject(g_hRespEvent, static_cast<DWORD>(remaining < 50 ? remaining : 50));
        }

        if (!gotResponse)
            split.PrintCmd("(no response within " + std::to_string(kTimeoutMs) + "ms)");
        else if (!pending.text.empty())
            split.PrintCmd(pending.text);
    }

    // ── Cleanup ────────────────────────────────────────────────────────────
    g_Running.store(false);
    if (g_hLogEvent) SetEvent(g_hLogEvent);

    logThread.join();
    hotThread.join();
    watchThread.join();

    CleanupSharedMemory();
    return 0;
}
