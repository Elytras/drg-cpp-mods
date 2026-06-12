// DrgCli.cpp — Combined CLI + injector host (DRG and RC).
//   • Creates all shared memory (log, cmd, response) and manages DLL lifecycle.
//   • Watches for the target process and auto-injects the matching mod DLL.
//   • Runs a readline REPL with tab-completion, ghost hints, and AC pane.
//
// Game selection at startup:
//   cli.exe          → auto-detect (falls back to DRG if neither runs)
//   cli.exe drg      → target FSD-Win64-Shipping.exe + DrgMods.dll
//   cli.exe rc       → target RogueCore-Win64-Shipping.exe + RcMods.dll
//
// All game-specific strings live in Profile.h/.cpp; this file reads them via
// the global `g_Profile` and never names a game directly.


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

#include "../SharedLib/core/IpcProtocol.h"
#include "Profile.h"
#include "../SharedLib/core/StringLib.h"

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

std::atomic<bool>           g_Running{ true };
std::atomic<bool>           g_Dumper7Loaded{ false };
std::atomic<InjectionState> g_InjState{ InjectionState::Watching };
HANDLE                      g_hProcess = NULL;
std::mutex                  g_InjectionMutex;

DWORD                       kWatchPollMs = 1000;

// Shared memory handles
HANDLE g_hLogEvent      = NULL;
HANDLE g_hCmdEvent      = NULL;
HANDLE g_hShutdownEvent = NULL;
HANDLE g_hRespEvent     = NULL;
HANDLE g_hDllReadyEvent = NULL;
HANDLE g_hLogMapping    = NULL;
HANDLE g_hCmdMapping    = NULL;
HANDLE g_hRespMapping   = NULL;
HANDLE g_hMetaMapping   = NULL;

std::mutex g_DllCommMutex;

LogBuffer*      g_pLogBuffer  = nullptr;
CommandBuffer*  g_pCmdBuffer  = nullptr;
ResponseBuffer* g_pRespBuffer = nullptr;
MetaBuffer*     g_pMetaBuffer = nullptr;

// DLL paths
std::wstring g_SourceDllPath;
std::wstring g_CopyDllPath;
FILETIME     g_LastInjectedTime{};

// CLI state
SplitConsole*              g_pSplit = nullptr;
std::vector<KnownFunction> g_KnownFunctions;
std::vector<KnownCommand>  g_KnownCommands;

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
    if (cmd == "reload")
    {
        // Clear a prior 'unload' suppression so an explicit reload re-injects — but ONLY
        // when Suppressed. Do NOT clobber Active → Watching: that makes DllActive() false,
        // so ReloadDLL skips the copy and UnloadDLL no-ops, leaving the adopted DLL to be
        // re-adopted instead of actually reloaded.
        InjectionState expected = InjectionState::Suppressed;
        g_InjState.compare_exchange_strong(expected, InjectionState::Watching);
        ReloadDLL();                                  // copy-first; keeps the mod if the source is still locked
        return 0;
    }
    if (cmd == "launch") { LaunchGame(); return 0; }
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
    // If the previous response timed out, hasCommand may be stuck true because
    // the dead DLL never cleared it.  Cap the spin so we don't deadlock.
    {
        const DWORD64 slotDeadline = GetTickCount64() + 500;
        while (g_pCmdBuffer->hasCommand.load(std::memory_order_acquire))
        {
            if (GetTickCount64() >= slotDeadline)
            {
                g_pCmdBuffer->hasCommand.store(false, std::memory_order_release);
                break;
            }
            Sleep(10);
        }
    }
    // Drop any stale, unread response (e.g. from a previous timed-out wait).
    // Without this, the DLL's SendResponse will block waiting for ready=false
    // and never write our response.
    if (g_pRespBuffer && g_pRespBuffer->ready.load(std::memory_order_acquire))
    {
        g_pRespBuffer->ready.store(false, std::memory_order_release);
        if (g_hRespEvent) ResetEvent(g_hRespEvent);
    }
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
//  Auto-listcmds — fires whenever the DLL signals DRG_DllReady.
//
//  The DLL can't push a CommandsResponse on its own because the REPL only
//  consumes responses whose seq matches an outgoing request. So instead we
//  wait for a "DLL ready" signal and send `listcmds` from the CLI side; the
//  response then arrives with our own seq and goes through the normal
//  consumption path (which populates g_KnownCommands).
// ─────────────────────────────────────────────────────────────────────────────

// Discard any stale response sitting unread in the buffer. Must be called
// while holding g_DllCommMutex. Without this, the DLL's SendResponse blocks
// waiting for ready=false (because some previous response was never consumed)
// and silently times out — making all subsequent commands look hung.
static void ClearStaleResponse()
{
    if (!g_pRespBuffer) return;
    if (g_pRespBuffer->ready.load(std::memory_order_acquire))
    {
        g_pRespBuffer->ready.store(false, std::memory_order_release);
        if (g_hRespEvent) ResetEvent(g_hRespEvent);
    }
}

static void AutoSendListCmds()
{
    if (!g_pCmdBuffer || !g_pRespBuffer || !g_hCmdEvent || !g_hRespEvent) return;

    std::lock_guard<std::mutex> lock(g_DllCommMutex);

    // Wait for the cmd slot to free; if stuck (stale from a dead DLL), clear it.
    {
        const DWORD64 slotDeadline = GetTickCount64() + 500;
        while (g_pCmdBuffer->hasCommand.load(std::memory_order_acquire))
        {
            if (GetTickCount64() >= slotDeadline)
            {
                g_pCmdBuffer->hasCommand.store(false, std::memory_order_release);
                break;
            }
            Sleep(10);
        }
    }
    ClearStaleResponse();
    const uint32_t mySeq = g_pCmdBuffer->seq.fetch_add(1, std::memory_order_relaxed) + 1;
    strncpy_s(g_pCmdBuffer->command, "listcmds", sizeof(g_pCmdBuffer->command) - 1);
    g_pCmdBuffer->hasCommand.store(true, std::memory_order_release);
    SetEvent(g_hCmdEvent);

    // Drain the matching response. Same pattern as the REPL's wait loop.
    constexpr DWORD kTimeoutMs = 5000;
    const DWORD64 deadline = GetTickCount64() + kTimeoutMs;
    while (GetTickCount64() < deadline)
    {
        if (g_pRespBuffer->ready.load(std::memory_order_acquire) &&
            g_pRespBuffer->seq.load(std::memory_order_acquire) == mySeq)
        {
            (void)ConsumeResponse(g_pRespBuffer, g_hRespEvent);
            Log("Auto-loaded " + std::to_string(g_KnownCommands.size()) + " commands.");
            return;
        }
        const DWORD64 remaining = deadline - GetTickCount64();
        WaitForSingleObject(g_hRespEvent, static_cast<DWORD>(remaining < 50 ? remaining : 50));
    }
    Log("Auto-listcmds: no response within " + std::to_string(kTimeoutMs) + "ms");
}

// Persistent thread that watches DRG_DllReady. Each load (initial + hot-reload)
// signals the event; we wake up, reset it, and pull the fresh command list.
static void DllReadyThread()
{
    if (!g_hDllReadyEvent) return;
    while (g_Running.load(std::memory_order_relaxed))
    {
        const DWORD r = WaitForSingleObject(g_hDllReadyEvent, 500);

        // Bail before doing any work if we were woken (or timed out) during a
        // shutdown / profile switch. The switch SetEvents this event purely to
        // wake us so we can exit; without this check we'd fall into
        // AutoSendListCmds() and block ~5s waiting for a reply from the DLL that
        // was just unloaded.
        if (!g_Running.load(std::memory_order_relaxed))
        {
            if (g_hDllReadyEvent) ResetEvent(g_hDllReadyEvent);
            break;
        }

        if (r == WAIT_OBJECT_0)
        {
            ResetEvent(g_hDllReadyEvent);
            AutoSendListCmds();
        }
        // WAIT_TIMEOUT: loop back to re-check g_Running.
    }
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
#if Testing
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
#endif
// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
    system("chcp 65001 > nul");

    // ── Profile selection ──────────────────────────────────────────────────
    // First arg picks the game: "drg" or "rc". No arg → auto-detect by
    // scanning running processes; falls back to DRG if neither runs.
    const Profile* requested = nullptr;
    if (argc >= 2)
    {
        std::wstring wide = StringLib::ToWide(argv[1]);
        requested = ResolveProfile(wide.c_str());
        if (!requested)
        {
            std::cerr << "[CLI] Unknown profile '" << argv[1]
                      << "'. Use 'drg' or 'rc'. Aborting.\n";
            return 2;
        }
    }
    InitProfile(requested);

    // ── Split console ──────────────────────────────────────────────────────
    SplitConsole split;
    g_pSplit = &split;

    auto Print = [&](const std::string& s) { split.PrintCmd(s); };
    auto PrintBanner = [&]() {
        std::string banner = "--- ";
        banner += (g_Profile->id == GameId::RC) ? "RC" : "DRG";
        banner += " Mod CLI ---  Tab: complete  Right: accept hint  Up/Down: history";
        Print(banner);
    };
    PrintBanner();
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
    std::thread readyThread(DllReadyThread);

    Log("Ready. Watching for " + std::string(g_Profile->targetProcessNarrow) + "...");

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
    input.SetFilterCallbacks(
        [&](const std::string& s) { split.SetFilterUnderLock(s);  },
        [&]()                     { split.ClearFilterUnderLock(); }
    );
    input.SetMouseSelectionCallbacks(
        [&](COORD pos, bool rect) { split.OnMouseDownUnderLock(pos, rect); },
        [&](COORD pos)            { split.OnMouseDragUnderLock(pos); },
        [&](COORD pos)            { split.OnMouseUpUnderLock(pos); },
        [&]()                     { return split.CopySelectionUnderLock(); }
    );
    input.SetClearSelectionCallback([&]() { split.ClearSelectionUnderLock(); });
    input.SetRevealFilteredLineCallback(
        [&](COORD pos) { return split.RevealFilteredLineUnderLock(pos); });
    input.SetPeekCallbacks(
        [&](COORD pos) { return split.PeekLineUnderLock(pos); },
        [&]()          { return split.ClosePeekUnderLock(); },
        [&]()          { return split.IsPeekActive(); });
    input.SetAcMode(
        [&](int acH) -> SHORT { return split.SetAcHeightUnderLock(acH); },
        [&]()                 { split.ClearAcUnderLock(); }
    );

    input.SetDescriptionCallback([](const std::string& candidate) -> std::string {
        if (IEquals(candidate, "load"))     return "Load the DLL into the game process";
        if (IEquals(candidate, "unload"))   return "Unload the DLL from the game process";
        if (IEquals(candidate, "reload"))   return "Hot-reload the DLL";
        if (IEquals(candidate, "launch"))   return "Launch the game (profile args) + auto-inject";
        if (IEquals(candidate, "d7l"))      return "Load Dumper7 into the game process";
        if (IEquals(candidate, "d7u"))      return "Unload Dumper7 (in-game F6 only)";
        if (IEquals(candidate, "exit"))     return "Exit the CLI";
        if (IEquals(candidate, "killgame")) return "Kill the game process immediately";
        if (IEquals(candidate, "mode"))     return "Switch target game: mode drg | mode rc";
        if (IEquals(candidate, "mode drg")) return "Switch to DRG (FSD-Win64-Shipping.exe)";
        if (IEquals(candidate, "mode rc"))  return "Switch to RogueCore (RogueCore-Win64-Shipping.exe)";

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

    // ── Profile switcher ──────────────────────────────────────────────────
    // Shuts down all background threads, tears down shared memory, swaps the
    // profile, then brings everything back up — without relaunching the CLI.
    auto SwitchToProfile = [&](const Profile* newProfile)
    {
        if (g_Profile == newProfile)
        {
            Print(std::string("Already targeting ") + newProfile->targetProcessNarrow + ".");
            return;
        }

        const char* newName = (newProfile->id == GameId::RC) ? "RC" : "DRG";
        Print(std::string("Switching to ") + newName + "...");

        if (DllActive()) { UnloadDLL(false); Sleep(300); }

        // Wake and join all background threads before touching the handles.
        g_Running.store(false);
        if (g_hLogEvent)      SetEvent(g_hLogEvent);
        if (g_hDllReadyEvent) SetEvent(g_hDllReadyEvent);
        logThread.join();
        hotThread.join();
        watchThread.join();
        readyThread.join();

        CleanupSharedMemory();

        g_Profile = newProfile;
        g_KnownFunctions.clear();
        g_KnownCommands.clear();
        g_SourceDllPath.clear();
        g_CopyDllPath.clear();
        g_Dumper7Loaded.store(false);
        kWatchPollMs = 1000;

        g_Running.store(true);
        if (!InitSharedMemory())
        {
            Print("Failed to initialize shared memory for new profile. CLI is now inert.");
            return;
        }

        logThread   = std::thread(DllLogThread, g_pLogBuffer, g_hLogEvent);
        hotThread   = std::thread(HotReloadThread);
        watchThread = std::thread(ProcessWatcherThread);
        readyThread = std::thread(DllReadyThread);

        PrintBanner();
        Log(std::string("Switched to ") + newName + ". Watching for " + g_Profile->targetProcessNarrow + "...");
    };

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

        // Built-in: mode [drg|rc]
        if (IEquals(line, "mode") || IStartsWith(line, "mode "))
        {
            std::string arg = (line.size() > 5) ? line.substr(5) : "";
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());

            if (arg.empty())
            {
                Print(std::string("Current profile: ") +
                    ((g_Profile->id == GameId::RC) ? "RC" : "DRG") +
                    " (" + g_Profile->targetProcessNarrow + ")."
                    "  Use 'mode drg' or 'mode rc' to switch.");
            }
            else
            {
                std::wstring warg = StringLib::ToWide(arg);
                const Profile* newProfile = ResolveProfile(warg.c_str());
                if (!newProfile)
                    Print("Unknown profile '" + arg + "'. Use 'mode drg' or 'mode rc'.");
                else
                    SwitchToProfile(newProfile);
            }
            continue;
        }

        // Serialize the entire send+wait cycle vs the auto-listcmds thread so
        // their responses can't overwrite each other in the shared resp buffer.
        std::unique_lock<std::mutex> commLock(g_DllCommMutex);

        uint32_t mySeq = DispatchCommand(line);

        if (mySeq == 0) { commLock.unlock(); continue; } // built-in — handled internally

        if (!g_pRespBuffer || !g_hRespEvent)
        {
            commLock.unlock();
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

        if (!gotResponse && g_pCmdBuffer)
            g_pCmdBuffer->hasCommand.store(false, std::memory_order_release);

        commLock.unlock();

        if (!gotResponse)
            split.PrintCmd("(no response within " + std::to_string(kTimeoutMs) + "ms)");
        else if (!pending.text.empty())
            split.PrintCmd(pending.text);
    }

    // ── Cleanup ────────────────────────────────────────────────────────────
    g_Running.store(false);
    if (g_hLogEvent) SetEvent(g_hLogEvent);

    if (g_hDllReadyEvent) SetEvent(g_hDllReadyEvent); // wake DllReadyThread so it can exit
    logThread.join();
    hotThread.join();
    watchThread.join();
    readyThread.join();

    CleanupSharedMemory();
    return 0;
}
