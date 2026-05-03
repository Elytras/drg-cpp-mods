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

#include "../DrgMods/Common.h"
#include "../DrgMods/StringLib.h"
#include "SplitConsole.h"
#include "WinInput.h"

// ─────────────────────────────────────────────────────────────────────────────
//  CLI types
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

// ─────────────────────────────────────────────────────────────────────────────
//  Injection state machine
//   Watching   – auto-inject whenever the target process appears
//   Active     – DLL is currently loaded inside the target
//   Suppressed – "unload" was issued; watcher stays hands-off until "load"
// ─────────────────────────────────────────────────────────────────────────────

enum class InjectionState { Watching, Active, Suppressed };

static std::atomic<bool>           g_Running{ true };
static std::atomic<bool>           g_Dumper7Loaded{ false };
static std::atomic<InjectionState> g_InjState{ InjectionState::Watching };
static HANDLE                      g_hProcess = NULL;
static std::mutex                  g_InjectionMutex;

static bool DllActive()     { return g_InjState.load() == InjectionState::Active; }
static bool DllSuppressed() { return g_InjState.load() == InjectionState::Suppressed; }

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

static constexpr DWORD   kWatchPollMs = 1000;
static constexpr wchar_t kDumper7Path[] = LR"(D:\Repos\Dumper7\x64\Release\Dumper-7.dll)";

static const std::vector<std::string> k_BuiltinCmds = {
    "load", "unload", "reload", "d7l", "d7u", "exit"
};

// ─────────────────────────────────────────────────────────────────────────────
//  Logging  (direct to split console; falls back to stdout before UI init)
// ─────────────────────────────────────────────────────────────────────────────

static void Log(const std::string& msg)
{
    if (g_pSplit)
        g_pSplit->PrintLog("[INJ] " + msg);
    else
        std::cout << "[INJ] " << msg << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Process helpers
// ─────────────────────────────────────────────────────────────────────────────

static DWORD_PTR FindRemoteModuleByName(DWORD pid, const std::wstring& moduleName)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32W entry{ .dwSize = sizeof(entry) };
    DWORD_PTR result = 0;
    if (Module32FirstW(hSnap, &entry))
    {
        do {
            if (!_wcsicmp(entry.szModule, moduleName.c_str()))
            {
                result = reinterpret_cast<DWORD_PTR>(entry.hModule);
                break;
            }
        } while (Module32NextW(hSnap, &entry));
    }
    CloseHandle(hSnap);
    return result;
}

static DWORD GetProcId(const wchar_t* procName)
{
    DWORD  procId = 0;
    HANDLE hSnap  = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{ .dwSize = sizeof(entry) };
    if (Process32FirstW(hSnap, &entry))
    {
        do {
            if (!_wcsicmp(entry.szExeFile, procName))
            {
                procId = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &entry));
    }
    CloseHandle(hSnap);
    return procId;
}

static bool IsProcessAlive(HANDLE hProcess)
{
    if (!hProcess) return false;
    DWORD exitCode = 0;
    return GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  DLL path helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool SourceDllUpdated()
{
    if (g_SourceDllPath.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(g_SourceDllPath.c_str(), GetFileExInfoStandard, &info))
        return false;
    return CompareFileTime(&info.ftLastWriteTime, &g_LastInjectedTime) > 0;
}

static bool ResolveDllPaths()
{
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, modulePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(modulePath).parent_path();

#ifdef _DEBUG
    constexpr wchar_t kConfig[] = L"Debug";
#else
    constexpr wchar_t kConfig[] = L"Release";
#endif

    wchar_t relBuf[MAX_PATH];
    std::wstring relPath = std::wstring(L"..\\x64\\") + kConfig + L"\\" + DLL_FILENAME;
    GetFullPathNameW(relPath.c_str(), MAX_PATH, relBuf, nullptr);
    std::wstring firstCandidate = relBuf;

    if (GetFileAttributesW(relBuf) != INVALID_FILE_ATTRIBUTES)
    {
        g_SourceDllPath = relBuf;
    }
    else
    {
        std::wstring sideCandidate = (exeDir / DLL_FILENAME).wstring();
        if (GetFileAttributesW(sideCandidate.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            g_SourceDllPath = sideCandidate;
        }
        else
        {
            std::wcout << L"[INJ] DLL not found: " << firstCandidate
                << L" or " << sideCandidate << L"\n";
            return false;
        }
    }

    g_CopyDllPath = (exeDir /
        (std::filesystem::path(DLL_FILENAME).stem().wstring() + L"_copy.dll")).wstring();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Debug auto-attach  (Debug builds only)
// ─────────────────────────────────────────────────────────────────────────────

#ifdef _DEBUG
static void AttachDebugger(DWORD pid, DWORD timeoutMs = 15000)
{
    wchar_t cmdLine[128];
    swprintf_s(cmdLine, L"vsjitdebugger.exe -p %lu", pid);

    STARTUPINFOW        si{ .cb = sizeof(si) };
    PROCESS_INFORMATION pi{};

    Log("Debug: launching vsjitdebugger for PID " + std::to_string(pid) + "...");

    if (!CreateProcessW(nullptr, cmdLine, nullptr, nullptr,
        FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        Log("Debug: failed to launch vsjitdebugger (err " +
            std::to_string(GetLastError()) + ") — continuing without debugger.");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    Log("Debug: waiting for debugger to attach...");
    DWORD waited = 0;
    constexpr DWORD kPoll = 200;
    while (waited < timeoutMs)
    {
        Sleep(kPoll);
        waited += kPoll;

        HANDLE hTmp = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hTmp)
        {
            Log("Debug: target process disappeared while waiting for debugger.");
            return;
        }

        typedef LONG(WINAPI* NtQIP)(HANDLE, UINT, PVOID, ULONG, PULONG);
        static auto NtQueryInformationProcess =
            reinterpret_cast<NtQIP>(
                GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                    "NtQueryInformationProcess"));

        bool attached = false;
        if (NtQueryInformationProcess)
        {
            HANDLE debugPort = nullptr;
            LONG status = NtQueryInformationProcess(hTmp, 7,
                &debugPort, sizeof(debugPort), nullptr);
            attached = (status == 0 && debugPort != nullptr);
        }
        CloseHandle(hTmp);

        if (attached)
        {
            Log("Debug: debugger attached successfully.");
            return;
        }
    }
    Log("Debug: timed out waiting for debugger — continuing anyway.");
}
#endif // _DEBUG

// ─────────────────────────────────────────────────────────────────────────────
//  Shared memory  (created here; DLL opens/maps the same named objects)
// ─────────────────────────────────────────────────────────────────────────────

static bool InitSharedMemory()
{
    g_hLogEvent      = CreateEventW(NULL, FALSE, FALSE, EVENT_LOG_READY);
    g_hCmdEvent      = CreateEventW(NULL, FALSE, FALSE, EVENT_CMD_READY);
    g_hShutdownEvent = CreateEventW(NULL, TRUE,  FALSE, EVENT_SHUTDOWN);

    if (!g_hLogEvent || !g_hCmdEvent || !g_hShutdownEvent)
    {
        Log("Failed to create events (err " + std::to_string(GetLastError()) + ")");
        return false;
    }

    // Log buffer (DLL → host)
    g_hLogMapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(LogBuffer), SHMEM_LOGS);
    if (!g_hLogMapping) { Log("Failed to create log mapping"); return false; }
    bool logNew = (GetLastError() != ERROR_ALREADY_EXISTS);
    g_pLogBuffer = static_cast<LogBuffer*>(MapViewOfFile(g_hLogMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_pLogBuffer) { Log("Failed to map log buffer"); return false; }
    if (logNew) new (g_pLogBuffer) LogBuffer();

    // Command buffer (host → DLL)
    g_hCmdMapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(CommandBuffer), SHMEM_CMD);
    if (!g_hCmdMapping) { Log("Failed to create command mapping"); return false; }
    bool cmdNew = (GetLastError() != ERROR_ALREADY_EXISTS);
    g_pCmdBuffer = static_cast<CommandBuffer*>(MapViewOfFile(g_hCmdMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_pCmdBuffer) { Log("Failed to map command buffer"); return false; }
    if (cmdNew) new (g_pCmdBuffer) CommandBuffer();

    // Response buffer (DLL → CLI)
    g_hRespEvent = CreateEventW(NULL, TRUE, FALSE, EVENT_RESP_READY);
    if (!g_hRespEvent) { Log("Failed to create response event"); return false; }
    g_hRespMapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(ResponseBuffer), SHMEM_RESPONSE);
    if (!g_hRespMapping) { Log("Failed to create response mapping"); return false; }
    bool respNew = (GetLastError() != ERROR_ALREADY_EXISTS);
    g_pRespBuffer = static_cast<ResponseBuffer*>(MapViewOfFile(g_hRespMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_pRespBuffer) { Log("Failed to map response buffer"); return false; }
    if (respNew) new (g_pRespBuffer) ResponseBuffer();

    return true;
}

static void CleanupSharedMemory()
{
    if (g_pLogBuffer)  UnmapViewOfFile(g_pLogBuffer);
    if (g_pCmdBuffer)  UnmapViewOfFile(g_pCmdBuffer);
    if (g_pRespBuffer) UnmapViewOfFile(g_pRespBuffer);

    if (g_hLogMapping)    CloseHandle(g_hLogMapping);
    if (g_hCmdMapping)    CloseHandle(g_hCmdMapping);
    if (g_hRespMapping)   CloseHandle(g_hRespMapping);
    if (g_hLogEvent)      CloseHandle(g_hLogEvent);
    if (g_hCmdEvent)      CloseHandle(g_hCmdEvent);
    if (g_hShutdownEvent) CloseHandle(g_hShutdownEvent);
    if (g_hRespEvent)     CloseHandle(g_hRespEvent);

    g_pLogBuffer  = nullptr;
    g_pCmdBuffer  = nullptr;
    g_pRespBuffer = nullptr;

    g_hLogMapping    = NULL;
    g_hCmdMapping    = NULL;
    g_hRespMapping   = NULL;
    g_hLogEvent      = NULL;
    g_hCmdEvent      = NULL;
    g_hShutdownEvent = NULL;
    g_hRespEvent     = NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Inject / Unload
// ─────────────────────────────────────────────────────────────────────────────

static bool InjectDLL()
{
    std::lock_guard<std::mutex> lock(g_InjectionMutex);
    if (DllActive()) return true;
    if (!ResolveDllPaths()) return false;

    DWORD pid = GetProcId(TARGET_PROCESS);
    if (pid)
    {
        std::wstring copyFilename = std::filesystem::path(g_CopyDllPath).filename().wstring();
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (hSnap != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W entry{ .dwSize = sizeof(entry) };
            if (Module32FirstW(hSnap, &entry))
            {
                do {
                    if (!_wcsicmp(entry.szModule, copyFilename.c_str()))
                    {
                        Log("InjectDLL: DLL already mapped in target — adopting existing instance.");
                        g_hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
                        g_InjState.store(InjectionState::Active);
                        CloseHandle(hSnap);
                        return true;
                    }
                } while (Module32NextW(hSnap, &entry));
            }
            CloseHandle(hSnap);
        }
    }

    if (g_Dumper7Loaded.load())
    {
        DWORD pidCheck = GetProcId(TARGET_PROCESS);
        if (pidCheck)
        {
            std::wstring dumperName = std::filesystem::path(kDumper7Path).filename().wstring();
            if (FindRemoteModuleByName(pidCheck, dumperName))
            {
                Log("InjectDLL: aborting — Dumper7 is attached (unsafe to inject alongside it).");
                return false;
            }
            g_Dumper7Loaded.store(false);
        }
    }

    bool copied = false;
    for (int i = 0; i < 10; ++i)
    {
        if (CopyFileW(g_SourceDllPath.c_str(), g_CopyDllPath.c_str(), FALSE))
        {
            copied = true;
            break;
        }
        if (GetLastError() != ERROR_SHARING_VIOLATION) break;
        Log("DLL copy locked, retrying... (" + std::to_string(i + 1) + "/10)");
        Sleep(500);
    }
    if (!copied) { Log("Failed to copy DLL (err " + std::to_string(GetLastError()) + ")"); return false; }

    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(g_SourceDllPath.c_str(), GetFileExInfoStandard, &info))
        g_LastInjectedTime = info.ftLastWriteTime;

    pid = GetProcId(TARGET_PROCESS);
    if (!pid) { Log("Target process not found."); return false; }

#ifdef _DEBUG
    AttachDebugger(pid);
#endif

    g_hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!g_hProcess) { Log("Failed to open process. Run as Administrator."); return false; }

    void* loc = VirtualAllocEx(g_hProcess, nullptr,
        (g_CopyDllPath.size() + 1) * sizeof(wchar_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!loc)
    {
        Log("Memory allocation failed.");
        CloseHandle(g_hProcess); g_hProcess = NULL;
        return false;
    }

    WriteProcessMemory(g_hProcess, loc, g_CopyDllPath.c_str(),
        (g_CopyDllPath.size() + 1) * sizeof(wchar_t), nullptr);

    HANDLE hThread = CreateRemoteThread(g_hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(LoadLibraryW), loc, 0, nullptr);
    if (!hThread)
    {
        Log("CreateRemoteThread failed.");
        VirtualFreeEx(g_hProcess, loc, 0, MEM_RELEASE);
        CloseHandle(g_hProcess); g_hProcess = NULL;
        return false;
    }

    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(g_hProcess, loc, 0, MEM_RELEASE);

    if (exitCode == 0)
    {
        Log("LoadLibraryW returned NULL — injection failed.");
        CloseHandle(g_hProcess); g_hProcess = NULL;
        return false;
    }

    g_InjState.store(InjectionState::Active);
    Log("DLL injected successfully.");
    return true;
}

// suppress=true  → called by explicit "unload"; watcher goes Suppressed
// suppress=false → called internally (hot-reload, crash recovery); watcher stays Watching
static void UnloadDLL(bool suppress = false)
{
    std::lock_guard<std::mutex> lock(g_InjectionMutex);
    if (!DllActive()) { Log("UnloadDLL: not active, skipping."); return; }
    if (!g_hProcess)  { Log("UnloadDLL: no process handle.");   return; }

    if (!IsProcessAlive(g_hProcess))
    {
        Log("UnloadDLL: target process is no longer running.");
        CloseHandle(g_hProcess); g_hProcess = NULL;
        g_InjState.store(suppress ? InjectionState::Suppressed : InjectionState::Watching);
        return;
    }

    if (!g_hShutdownEvent)
    {
        Log("UnloadDLL: shutdown event handle is NULL.");
    }
    else
    {
        Log("UnloadDLL: signaling shutdown...");
        SetEvent(g_hShutdownEvent);

        HANDLE hDone = OpenEventW(SYNCHRONIZE, FALSE, EVENT_SHUTDOWN_DONE);
        if (hDone)
        {
            DWORD w = WaitForSingleObject(hDone, 5000);
            if (w == WAIT_OBJECT_0)     Log("UnloadDLL: DLL cleanup complete.");
            else if (w == WAIT_TIMEOUT) Log("UnloadDLL: timed out waiting for DLL cleanup.");
            else                        Log("UnloadDLL: wait failed: " + std::to_string(GetLastError()));
            CloseHandle(hDone);
        }
        else
        {
            Log("UnloadDLL: could not open done event (err " +
                std::to_string(GetLastError()) + "), waiting 2s as fallback.");
            Sleep(2000);
        }
        ResetEvent(g_hShutdownEvent);
    }

    DWORD pid = GetProcId(TARGET_PROCESS);
    if (!pid)
    {
        Log("UnloadDLL: could not find PID.");
        CloseHandle(g_hProcess); g_hProcess = NULL;
        g_InjState.store(suppress ? InjectionState::Suppressed : InjectionState::Watching);
        return;
    }

    std::wstring copyFilename = std::filesystem::path(g_CopyDllPath).filename().wstring();
    HMODULE      hRemoteModule = NULL;
    HANDLE       hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);

    if (hSnap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W entry{ .dwSize = sizeof(entry) };
        if (Module32FirstW(hSnap, &entry))
        {
            do {
                if (!_wcsicmp(entry.szModule, copyFilename.c_str()))
                {
                    hRemoteModule = entry.hModule;
                    break;
                }
            } while (Module32NextW(hSnap, &entry));
        }
        CloseHandle(hSnap);
    }

    if (!hRemoteModule)
    {
        Log("UnloadDLL: DLL copy not found in target — already unloaded?");
        CloseHandle(g_hProcess); g_hProcess = NULL;
        g_InjState.store(suppress ? InjectionState::Suppressed : InjectionState::Watching);
        return;
    }

    HANDLE hThread = CreateRemoteThread(g_hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(FreeLibrary),
        hRemoteModule, 0, nullptr);

    if (!hThread)
    {
        Log("UnloadDLL: CreateRemoteThread failed: " + std::to_string(GetLastError()));
    }
    else
    {
        DWORD w = WaitForSingleObject(hThread, 5000);
        if (w == WAIT_OBJECT_0)
        {
            DWORD ec = 0;
            GetExitCodeThread(hThread, &ec);
            Log(ec ? "UnloadDLL: FreeLibrary succeeded." : "UnloadDLL: FreeLibrary returned FALSE.");
        }
        else if (w == WAIT_TIMEOUT)
        {
            Log("UnloadDLL: FreeLibrary thread timed out.");
        }
        CloseHandle(hThread);
    }

    CloseHandle(g_hProcess); g_hProcess = NULL;
    g_InjState.store(suppress ? InjectionState::Suppressed : InjectionState::Watching);
    Log("UnloadDLL: complete.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Dumper7 attachment
// ─────────────────────────────────────────────────────────────────────────────

static bool LoadDumper7()
{
    std::lock_guard<std::mutex> lock(g_InjectionMutex);

    DWORD pid = GetProcId(TARGET_PROCESS);
    if (!pid) { Log("Dumper7: target process not found."); return false; }

    std::wstring dumperName = std::filesystem::path(kDumper7Path).filename().wstring();
    if (g_Dumper7Loaded.load())
    {
        if (FindRemoteModuleByName(pid, dumperName))
        {
            Log("Dumper7: already marked as loaded.");
            return true;
        }
        Log("Dumper7: marked as loaded but not found in target; correcting state.");
        g_Dumper7Loaded.store(false);
    }

    if (!g_CopyDllPath.empty())
    {
        std::wstring usualName = std::filesystem::path(g_CopyDllPath).filename().wstring();
        if (FindRemoteModuleByName(pid, usualName))
        {
            Log("Dumper7: aborting — usual DLL is present in target.");
            return false;
        }
    }

    if (FindRemoteModuleByName(pid, dumperName))
    {
        g_Dumper7Loaded.store(true);
        Log("Dumper7: already present in target; marked as loaded.");
        return true;
    }

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) { Log("Dumper7: OpenProcess failed."); return false; }

    size_t sizeBytes = (wcslen(kDumper7Path) + 1) * sizeof(wchar_t);
    void* remoteBuf = VirtualAllocEx(hProcess, nullptr, sizeBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBuf)
    {
        Log("Dumper7: VirtualAllocEx failed.");
        CloseHandle(hProcess);
        return false;
    }

    if (!WriteProcessMemory(hProcess, remoteBuf, kDumper7Path, static_cast<SIZE_T>(sizeBytes), nullptr))
    {
        Log("Dumper7: WriteProcessMemory failed.");
        VirtualFreeEx(hProcess, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(LoadLibraryW), remoteBuf, 0, nullptr);
    if (!hThread)
    {
        Log("Dumper7: CreateRemoteThread failed.");
        VirtualFreeEx(hProcess, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteBuf, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    if (exitCode == 0) { Log("Dumper7: LoadLibraryW returned NULL — load failed."); return false; }

    g_Dumper7Loaded.store(true);
    Log("Dumper7: loaded (attach-and-forget).");
    return true;
}

static bool UnloadDumper7()
{
    Log("Dumper7: external unload disabled — press F6 in-game to unload.");
    return false;
}

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
//  Completion / hint helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool IStartsWith(const std::string& str, const std::string& prefix)
{
    if (prefix.size() > str.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (tolower((unsigned char)str[i]) != tolower((unsigned char)prefix[i])) return false;
    return true;
}

static bool IEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

// Parse the buffer for the 'call' command.
// Commit convention: the FIRST "::" token after "call" is the func/args delimiter.
struct CallParse
{
    enum class Mode { TopLevel, FuncName, Args } mode = Mode::TopLevel;

    std::string funcPrefix;   // partial func name (FuncName mode)
    std::string funcName;     // committed func name (Args mode)
    int         argIndex = 0;
    std::string argPrefix;
    std::string beforeToken;
    std::string currentToken;
};

// Strips a single surrounding quote pair from a function prefix typed by the user.
// e.g. `"Fix Wid` → {"Fix Wid", '"'},  `Fix` → {"Fix", 0}
static std::pair<std::string, char> StripFuncQuotes(const std::string& s)
{
    if (s.empty()) return { s, 0 };
    char q = s.front();
    if (q != '"' && q != '\'') return { s, 0 };
    std::string stripped = s.substr(1);
    if (!stripped.empty() && stripped.back() == q)
        stripped.pop_back();
    return { stripped, q };
}

// Splits on the LAST "::" to separate an optional owner-filter prefix from the name part.
// "BP_Player_C::Fix"                → {"BP_Player_C",             "Fix"}
// "BP_Player_C::BP_Player_0::Fix W" → {"BP_Player_C::BP_Player_0", "Fix W"}
// "FixWidgets"                       → {"",                         "FixWidgets"}
static std::pair<std::string, std::string> SplitOwnerPrefix(const std::string& s)
{
    size_t pos = s.rfind("::");
    if (pos == std::string::npos) return { "", s };
    return { s.substr(0, pos), s.substr(pos + 2) };
}

// Checks whether fn matches an ownerPart::namePart query.
// ownerPart may be "Class::Instance" — only its first segment is compared against fn.owner.
static bool MatchesFunction(const KnownFunction& fn,
    const std::string& ownerPart, const std::string& namePart)
{
    if (!IStartsWith(fn.name, namePart)) return false;
    if (ownerPart.empty()) return true;
    size_t colonPos = ownerPart.find("::");
    const std::string classFilter = (colonPos == std::string::npos)
        ? ownerPart : ownerPart.substr(0, colonPos);
    return IStartsWith(fn.owner, classFilter);
}

static CallParse ParseBuffer(const std::string& buf)
{
    CallParse p;

    std::vector<std::string> words;
    std::vector<size_t>      starts;
    {
        size_t i = 0;
        while (i <= buf.size())
        {
            size_t sp = buf.find(' ', i);
            if (sp == std::string::npos) sp = buf.size();
            words.push_back(buf.substr(i, sp - i));
            starts.push_back(i);
            i = sp + 1;
            if (sp == buf.size()) break;
        }
    }

    if (words.empty()) { p.mode = CallParse::Mode::TopLevel; return p; }

    if (words.size() == 1 || words[0] != "call")
    {
        size_t last = words.size() - 1;
        p.mode = CallParse::Mode::TopLevel;
        p.currentToken = words[last];
        p.beforeToken = buf.substr(0, starts[last]);
        return p;
    }

    size_t sepIdx = std::string::npos;
    for (size_t i = 1; i < words.size(); ++i)
        if (words[i] == "::") { sepIdx = i; break; }

    if (sepIdx == std::string::npos)
    {
        p.mode = CallParse::Mode::FuncName;
        p.funcPrefix = buf.substr(starts[1]);
        p.currentToken = p.funcPrefix;
        p.beforeToken = buf.substr(0, starts[1]);
    }
    else
    {
        std::string fnPart;
        for (size_t i = 1; i < sepIdx; ++i)
        {
            if (i > 1) fnPart += ' ';
            fnPart += words[i];
        }

        p.mode = CallParse::Mode::Args;
        p.funcName = SplitOwnerPrefix(StripFuncQuotes(fnPart).first).second;

        size_t last = words.size() - 1;
        p.argIndex = std::max(0, (int)(last - sepIdx) - 1);
        if (last == sepIdx)
        {
            p.argIndex = 0;
            p.currentToken = "";
            p.beforeToken = buf;
        }
        else
        {
            p.argPrefix = words[last];
            p.currentToken = words[last];
            p.beforeToken = buf.substr(0, starts[last]);
        }
    }

    return p;
}

static std::vector<std::string> SplitParams(const std::string& params)
{
    std::vector<std::string> result;
    if (params.empty()) return result;
    size_t i = 0;
    while (i < params.size())
    {
        size_t comma = params.find(',', i);
        if (comma == std::string::npos) comma = params.size();
        std::string p = params.substr(i, comma - i);
        while (!p.empty() && p.front() == ' ') p = p.substr(1);
        while (!p.empty() && p.back()  == ' ') p.pop_back();
        if (!p.empty()) result.push_back(p);
        i = comma + 1;
    }
    return result;
}

static void CompletionCallback(const std::string& buf, std::vector<std::string>& out)
{
    auto p = ParseBuffer(buf);

    if (p.mode == CallParse::Mode::TopLevel)
    {
        for (const auto& cmd : k_BuiltinCmds)
            if (IStartsWith(cmd, p.currentToken))
                out.push_back(p.beforeToken + cmd);

        for (const auto& kc : g_KnownCommands)
            if (IStartsWith(kc.name, p.currentToken))
                out.push_back(p.beforeToken + kc.name);
    }
    else if (p.mode == CallParse::Mode::FuncName)
    {
        auto sq = StripFuncQuotes(p.funcPrefix);
        const std::string& stripped = sq.first;
        const char quoteChar = sq.second;
        auto op = SplitOwnerPrefix(stripped);
        const std::string& ownerPart = op.first;
        const std::string& namePart  = op.second;
        for (const auto& fn : g_KnownFunctions)
        {
            if (!MatchesFunction(fn, ownerPart, namePart)) continue;
            std::string fullRef = ownerPart.empty() ? fn.name : ownerPart + "::" + fn.name;
            std::string quoted = quoteChar
                ? std::string(1, quoteChar) + fullRef + quoteChar
                : fullRef;
            if (IEquals(fn.name, namePart))
                out.push_back(p.beforeToken + quoted + " :: ");
            else
                out.push_back(p.beforeToken + quoted);
        }
    }
    // Mode::Args — no completion for argument values
}

static constexpr WORD kHintColor = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

static WinHint HintCallback(const std::string& buf, int cursor)
{
    if (buf.empty() || cursor < 0) return {};

    const std::string atCursor = buf.substr(0, cursor);
    auto p = ParseBuffer(atCursor);

    auto makeNameHint = [&](const std::string& fullName,
        size_t             prefixLen,
        const std::string& params,
        const std::string& owner,
        size_t             matchCount) -> WinHint
        {
            WinHint h;
            h.color = kHintColor;

            std::string nameRemainder = (prefixLen < fullName.size())
                ? fullName.substr(prefixLen) : "";

            int ahead = (int)buf.size() - cursor;
            if (ahead <= 0)
            {
                h.inlineText = nameRemainder;
            }
            else if (!nameRemainder.empty())
            {
                int overlap = std::min((int)nameRemainder.size(), ahead);
                bool matches = true;
                for (int i = 0; i < overlap && matches; ++i)
                    if (tolower((unsigned char)buf[cursor + i]) !=
                        tolower((unsigned char)nameRemainder[i])) matches = false;
                if (matches) h.inlineText = nameRemainder.substr(0, overlap);
            }

            if (matchCount == 1)
            {
                if (!params.empty()) h.appendText += "  (" + params + ")";
                if (!owner.empty())  h.appendText += "  [" + owner + "]";
            }
            else
            {
                h.appendText = "  +" + std::to_string(matchCount - 1) + " more";
            }
            return h;
        };

    if (p.mode == CallParse::Mode::TopLevel && !p.currentToken.empty())
    {
        std::vector<std::string> builtinMatches;
        for (const auto& cmd : k_BuiltinCmds)
            if (IStartsWith(cmd, p.currentToken)) builtinMatches.push_back(cmd);

        std::vector<const KnownCommand*> cmdMatches;
        for (const auto& kc : g_KnownCommands)
            if (IStartsWith(kc.name, p.currentToken)) cmdMatches.push_back(&kc);

        if (!builtinMatches.empty())
            return makeNameHint(builtinMatches[0], p.currentToken.size(), "", "", builtinMatches.size());
        if (!cmdMatches.empty())
            return makeNameHint(cmdMatches[0]->name, p.currentToken.size(),
                cmdMatches[0]->params, "", cmdMatches.size());
        return {};
    }

    if (p.mode == CallParse::Mode::FuncName && !p.funcPrefix.empty())
    {
        auto sq = StripFuncQuotes(p.funcPrefix);
        const std::string& stripped = sq.first;
        auto op = SplitOwnerPrefix(stripped);
        const std::string& ownerPart = op.first;
        const std::string& namePart  = op.second;
        std::vector<const KnownFunction*> matches;
        for (const auto& fn : g_KnownFunctions)
            if (MatchesFunction(fn, ownerPart, namePart)) matches.push_back(&fn);
        if (matches.empty()) return {};
        return makeNameHint(matches[0]->name, namePart.size(),
            matches[0]->params, matches[0]->owner, matches.size());
    }

    if (p.mode == CallParse::Mode::Args)
    {
        const KnownFunction* fn = nullptr;
        for (const auto& kf : g_KnownFunctions)
            if (IEquals(kf.name, p.funcName)) { fn = &kf; break; }
        if (!fn || fn->params.empty()) return {};

        auto paramList = SplitParams(fn->params);
        if (p.argIndex >= (int)paramList.size()) return {};

        WinHint h;
        h.color = kHintColor;
        for (int i = p.argIndex; i < (int)paramList.size(); ++i)
        {
            if (i > p.argIndex) h.appendText += "  ";
            h.appendText += (i == p.argIndex)
                ? "[" + paramList[i] + "]"
                : paramList[i];
        }
        return h;
    }

    return {};
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
//  DLL log reader thread
//  Reads from the DLL's shared log ring buffer starting at the current write
//  position (live logs only, no backlog).
// ─────────────────────────────────────────────────────────────────────────────

static void DllLogThread(LogBuffer* pLog, HANDLE hLogEvent)
{
    uint32_t myReadPos = pLog->writePos.load(std::memory_order_acquire);
    char lineBuf[1024];
    size_t linePos = 0;

    while (g_Running)
    {
        WaitForSingleObject(hLogEvent, 100);

        while (true)
        {
            uint32_t writePos = pLog->writePos.load(std::memory_order_acquire);
            if (myReadPos == writePos) break;

            char c = pLog->data[myReadPos % LOG_BUFFER_SIZE];
            ++myReadPos;

            if (c == '\n' || linePos >= sizeof(lineBuf) - 1)
            {
                lineBuf[linePos] = '\0';
                if (linePos > 0 && g_pSplit)
                    g_pSplit->PrintLog(std::string("[DLL] ") + lineBuf);
                linePos = 0;
            }
            else
            {
                lineBuf[linePos++] = c;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Background threads
// ─────────────────────────────────────────────────────────────────────────────

static void HotReloadThread()
{
    while (g_Running)
    {
        Sleep(kWatchPollMs);
        if (!DllActive() || g_SourceDllPath.empty()) continue;

        if (SourceDllUpdated())
        {
            Log("HotReload: newer DLL detected, reloading...");
            UnloadDLL(false);
            Sleep(300);
            InjectDLL();
        }
    }
}

static void ProcessWatcherThread()
{
    while (g_Running)
    {
        Sleep(kWatchPollMs);

        InjectionState state = g_InjState.load();

        if (state == InjectionState::Suppressed) continue;

        if (state == InjectionState::Active)
        {
            if (!IsProcessAlive(g_hProcess))
            {
                Log("ProcessWatcher: target process exited unexpectedly — waiting for relaunch...");
                if (g_hProcess) { CloseHandle(g_hProcess); g_hProcess = NULL; }
                g_InjState.store(InjectionState::Watching);
            }
            continue;
        }

        // Watching: look for target and inject
        DWORD pid = GetProcId(TARGET_PROCESS);
        if (!pid) continue;

        Log("ProcessWatcher: target process found (PID " + std::to_string(pid) + "), injecting...");
        if (InjectDLL())
            Log("ProcessWatcher: auto-inject succeeded.");
        else
            Log("ProcessWatcher: inject failed, will retry...");
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
        if (IEquals(candidate, "exit"))   return "Exit the CLI";

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
