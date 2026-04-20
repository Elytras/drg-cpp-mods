#define _CRT_SECURE_NO_WARNINGS
#define VERBOSE 0

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <mutex>
#include <thread>

#include "../DrgMods/Common.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────────────────────────────────────

// Injection state machine
//   Watching   – auto-inject whenever the target process appears
//   Active     – DLL is currently loaded inside the target
//   Suppressed – explicit "unload" was issued; watcher stays hands-off
//                until a manual "load" re-enables watching
enum class InjectionState { Watching, Active, Suppressed };

std::atomic<bool>           g_Running{ true };
std::atomic<InjectionState> g_InjState{ InjectionState::Watching };
HANDLE                      g_hProcess = NULL;

// Convenience helpers (keep call-sites readable)
static bool DllActive() { return g_InjState.load() == InjectionState::Active; }
static bool DllSuppressed() { return g_InjState.load() == InjectionState::Suppressed; }

// Events
HANDLE g_hLogEvent = NULL;
HANDLE g_hCmdEvent = NULL;
HANDLE g_hShutdownEvent = NULL;
HANDLE g_hRespEvent = NULL;

// Mappings
HANDLE g_hLogMapping = NULL;
HANDLE g_hCmdMapping = NULL;
HANDLE g_hRespMapping = NULL;

// Buffers
LogBuffer* g_pLogBuffer = nullptr;
CommandBuffer* g_pCmdBuffer = nullptr;
ResponseBuffer* g_pRespBuffer = nullptr;
std::mutex g_InjectionMutex;

// DLL paths
std::wstring g_SourceDllPath;      // original DLL we watch for changes
std::wstring g_CopyDllPath;        // working copy we actually inject
FILETIME     g_LastInjectedTime{}; // write-time of the source at last inject

constexpr int  kTimeoutMs = 3000;
constexpr DWORD kWatchPollMs = 1000; // how often ProcessWatcherThread polls

// ─────────────────────────────────────────────────────────────────────────────
//  Logging
// ─────────────────────────────────────────────────────────────────────────────

static void Log(const std::string& msg)
{
    std::cout << "[Injector] " << msg << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Process helpers
// ─────────────────────────────────────────────────────────────────────────────

static DWORD GetProcId(const wchar_t* procName)
{
    DWORD  procId = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
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
            std::wcout << L"[Injector] DLL not found: " << firstCandidate
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
// Launches the VS JIT debugger against `pid` and waits up to `timeoutMs` for
// a debugger to become attached to the target process.
static void AttachDebugger(DWORD pid, DWORD timeoutMs = 15000)
{
    // vsjitdebugger.exe is on PATH when VS build tools are installed;
    // it prompts the "Which debugger?" dialog then attaches.
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

    // Poll until IsDebuggerPresent() returns true inside the target or we time out.
    Log("Debug: waiting for debugger to attach...");
    DWORD waited = 0;
    constexpr DWORD kPoll = 200;
    while (waited < timeoutMs)
    {
        Sleep(kPoll);
        waited += kPoll;

        // Open a fresh handle each poll — the target process must be alive.
        HANDLE hTmp = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hTmp)
        {
            Log("Debug: target process disappeared while waiting for debugger.");
            return;
        }

        // Read the remote PEB.BeingDebugged byte via NtQueryInformationProcess.
        // Simpler approach: just check if a debug port exists.
        typedef LONG(WINAPI* NtQIP)(HANDLE, UINT, PVOID, ULONG, PULONG);
        static auto NtQueryInformationProcess =
            reinterpret_cast<NtQIP>(
                GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                    "NtQueryInformationProcess"));

        bool attached = false;
        if (NtQueryInformationProcess)
        {
            HANDLE debugPort = nullptr;
            // ProcessDebugPort = 7
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
//  Shared memory
// ─────────────────────────────────────────────────────────────────────────────

static bool InitSharedMemory()
{
    g_hLogEvent = CreateEventW(NULL, FALSE, FALSE, EVENT_LOG_READY);
    g_hCmdEvent = CreateEventW(NULL, FALSE, FALSE, EVENT_CMD_READY);
    g_hShutdownEvent = CreateEventW(NULL, TRUE, FALSE, EVENT_SHUTDOWN);

    if (!g_hLogEvent || !g_hCmdEvent || !g_hShutdownEvent)
    {
        Log("Failed to create events (err " + std::to_string(GetLastError()) + ")");
        return false;
    }

    // Log buffer
    g_hLogMapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(LogBuffer), SHMEM_LOGS);
    if (!g_hLogMapping) { Log("Failed to create log mapping (err " + std::to_string(GetLastError()) + ")"); return false; }
    bool logNew = (GetLastError() != ERROR_ALREADY_EXISTS);
    g_pLogBuffer = static_cast<LogBuffer*>(MapViewOfFile(g_hLogMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_pLogBuffer) { Log("Failed to map log buffer (err " + std::to_string(GetLastError()) + ")"); return false; }
    if (logNew) new (g_pLogBuffer) LogBuffer();

    // Command buffer
    g_hCmdMapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(CommandBuffer), SHMEM_CMD);
    if (!g_hCmdMapping) { Log("Failed to create command mapping (err " + std::to_string(GetLastError()) + ")"); return false; }
    bool cmdNew = (GetLastError() != ERROR_ALREADY_EXISTS);
    g_pCmdBuffer = static_cast<CommandBuffer*>(MapViewOfFile(g_hCmdMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_pCmdBuffer) { Log("Failed to map command buffer (err " + std::to_string(GetLastError()) + ")"); return false; }
    if (cmdNew) new (g_pCmdBuffer) CommandBuffer();

    // Response buffer
    g_hRespEvent = CreateEventW(NULL, TRUE, FALSE, EVENT_RESP_READY);
    if (!g_hRespEvent) { Log("Failed to create response event (err " + std::to_string(GetLastError()) + ")"); return false; }
    g_hRespMapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(ResponseBuffer), SHMEM_RESPONSE);
    if (!g_hRespMapping) { Log("Failed to create response mapping (err " + std::to_string(GetLastError()) + ")"); return false; }
    bool respNew = (GetLastError() != ERROR_ALREADY_EXISTS);
    g_pRespBuffer = static_cast<ResponseBuffer*>(MapViewOfFile(g_hRespMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_pRespBuffer) { Log("Failed to map response buffer (err " + std::to_string(GetLastError()) + ")"); return false; }
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

    g_pLogBuffer = nullptr;
    g_pCmdBuffer = nullptr;
    g_pRespBuffer = nullptr;

    g_hLogMapping = NULL;
    g_hCmdMapping = NULL;
    g_hRespMapping = NULL;
    g_hLogEvent = NULL;
    g_hCmdEvent = NULL;
    g_hShutdownEvent = NULL;
    g_hRespEvent = NULL;
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
    // Copy the DLL so the compiler can freely overwrite the original
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

// suppress=true  → called by explicit "unload" command; watcher goes Suppressed
// suppress=false → called internally (hot-reload, crash recovery); watcher stays Watching
static void UnloadDLL(bool suppress = false)
{
    std::lock_guard<std::mutex> lock(g_InjectionMutex);
    if (!DllActive()) { Log("UnloadDLL: not active, skipping.");  return; }
    if (!g_hProcess) { Log("UnloadDLL: no process handle.");     return; }

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
            if (w == WAIT_OBJECT_0)      Log("UnloadDLL: DLL cleanup complete.");
            else if (w == WAIT_TIMEOUT)  Log("UnloadDLL: timed out waiting for DLL cleanup.");
            else                         Log("UnloadDLL: wait failed: " + std::to_string(GetLastError()));
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
        Log("UnloadDLL: DLL copy not found in target process — already unloaded?");
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
//  Background threads
// ─────────────────────────────────────────────────────────────────────────────

static void LogReaderThread()
{
    char   lineBuffer[1024];
    size_t linePos = 0;

    while (g_Running)
    {
        WaitForSingleObject(g_hLogEvent, 100);
        if (!g_pLogBuffer) continue;

        while (true)
        {
            uint32_t writePos = g_pLogBuffer->writePos.load(std::memory_order_acquire);
            uint32_t readPos = g_pLogBuffer->readPos.load(std::memory_order_acquire);
            if (readPos == writePos) break;

            char c = g_pLogBuffer->data[readPos % LOG_BUFFER_SIZE];
            g_pLogBuffer->readPos.store(readPos + 1, std::memory_order_release);

            if (c == '\n' || linePos >= sizeof(lineBuffer) - 1)
            {
                lineBuffer[linePos] = '\0';
                std::cout << "[DLL] " << lineBuffer << "\n";
                linePos = 0;
            }
            else
            {
                lineBuffer[linePos++] = c;
            }
        }
    }
}

static void CommandThread()
{
    while (g_Running)
    {
        Sleep(50);
        if (!g_pCmdBuffer) continue;
        if (!g_pCmdBuffer->hasCommand.load(std::memory_order_acquire)) continue;

        std::string cmd(g_pCmdBuffer->command);

        if (cmd == "load")
        {
            if (DllActive())
            {
                Log("DLL already loaded.");
            }
            else
            {
                // Re-enable auto-injection (clears Suppressed if set), then inject now.
                g_InjState.store(InjectionState::Watching);
                InjectDLL();
            }
        }
        else if (cmd == "unload")
        {
            if (DllActive())
                UnloadDLL(/*suppress=*/true);
            else
            {
                // Not active, but still mark as suppressed so the watcher doesn't
                // auto-inject the moment it sees the process.
                g_InjState.store(InjectionState::Suppressed);
                Log("DLL not loaded; auto-injection suppressed until next 'load'.");
            }
        }
        else if (cmd == "reload")
        {
            // Explicit reload: unload internally (keep Watching), then inject.
            UnloadDLL(/*suppress=*/false);
            Sleep(500);
            InjectDLL();
        }
        else
        {
            if (DllActive())
            {
#if VERBOSE
                Log("Forwarding command to DLL: " + cmd);
#endif
                SetEvent(g_hCmdEvent);
                Sleep(50);
            }
            else
            {
                Log("Cannot forward command — DLL not loaded.");
            }
        }

        g_pCmdBuffer->hasCommand.store(false, std::memory_order_release);
    }
}

// Polls the source DLL for changes and auto-reloads when a newer build appears
static void HotReloadThread()
{
    while (g_Running)
    {
        Sleep(kWatchPollMs);
        if (!DllActive() || g_SourceDllPath.empty()) continue;

        if (SourceDllUpdated())
        {
            Log("HotReload: newer DLL detected, reloading...");
            UnloadDLL();
            Sleep(300);
            InjectDLL();
        }
    }
}

// Continuously watches for the target process, respecting the injection state:
//   Active     – monitors for unexpected exit; on crash → Watching (auto-reinject)
//   Watching   – polls until process appears, then injects
//   Suppressed – hands-off; only exits this state via an explicit "load" command
static void ProcessWatcherThread()
{
    while (g_Running)
    {
        Sleep(kWatchPollMs);

        InjectionState state = g_InjState.load();

        if (state == InjectionState::Suppressed)
        {
            // Hands-off until the user issues "load".
            continue;
        }

        if (state == InjectionState::Active)
        {
            // Monitor for unexpected exit (crash / force-kill).
            if (!IsProcessAlive(g_hProcess))
            {
                Log("ProcessWatcher: target process exited unexpectedly — "
                    "waiting for relaunch...");
                if (g_hProcess) { CloseHandle(g_hProcess); g_hProcess = NULL; }
                // Transition to Watching so we auto-reinject on next launch.
                g_InjState.store(InjectionState::Watching);
            }
            continue;
        }

        // state == Watching: look for the target process and inject.
        DWORD pid = GetProcId(TARGET_PROCESS);
        if (!pid) continue;

        Log("ProcessWatcher: target process found (PID " +
            std::to_string(pid) + "), injecting...");

        if (InjectDLL())
        {
            Log("ProcessWatcher: auto-inject succeeded.");
        }
        else
        {
            Log("ProcessWatcher: inject failed, will retry...");
            // Stay in Watching so next poll retries.
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    system("chcp 65001 > nul");
    Log("Starting up...");

    if (!InitSharedMemory())
    {
        Log("Failed to initialize shared memory. Exiting.");
        return 1;
    }

    std::thread logThread(LogReaderThread);
    std::thread cmdThread(CommandThread);
    std::thread hotThread(HotReloadThread);
    std::thread watchThread(ProcessWatcherThread);

    Log("Ready. Watching for " + std::string(TARGET_PROCESS_NARROW) + "...");

    while (g_Running)
    {
        Sleep(100);
        // All process-lifecycle logic lives in ProcessWatcherThread.
    }

    Log("Shutting down...");
    if (DllActive()) UnloadDLL(/*suppress=*/false);

    g_Running = false;
    if (g_hLogEvent) SetEvent(g_hLogEvent);

    logThread.join();
    cmdThread.join();
    hotThread.join();
    watchThread.join();

    CleanupSharedMemory();
    Log("Goodbye.");
    return 0;
}