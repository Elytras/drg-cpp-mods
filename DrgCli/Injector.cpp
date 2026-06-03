// Injector.cpp — DLL injection, shared memory, Dumper7, and background watcher threads.

#define _CRT_SECURE_NO_WARNINGS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <iostream>

#include "../SharedLib/IpcProtocol.h"
#include "CliTypes.h"
#include "Injector.h"
#include "Profile.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Process helpers
// ─────────────────────────────────────────────────────────────────────────────

DWORD_PTR FindRemoteModuleByName(DWORD pid, const std::wstring& moduleName)
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

DWORD GetProcId(const wchar_t* procName)
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

bool IsProcessAlive(HANDLE hProcess)
{
    if (!hProcess) return false;
    DWORD exitCode = 0;
    return GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  DLL path helpers
// ─────────────────────────────────────────────────────────────────────────────

bool SourceDllUpdated()
{
    if (g_SourceDllPath.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(g_SourceDllPath.c_str(), GetFileExInfoStandard, &info))
        return false;
    return CompareFileTime(&info.ftLastWriteTime, &g_LastInjectedTime) > 0;
}

bool ResolveDllPaths()
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
    std::wstring relPath = std::wstring(L"..\\x64\\") + kConfig + L"\\" + g_Profile->dllFilename;
    GetFullPathNameW(relPath.c_str(), MAX_PATH, relBuf, nullptr);
    std::wstring firstCandidate = relBuf;

    if (GetFileAttributesW(relBuf) != INVALID_FILE_ATTRIBUTES)
    {
        g_SourceDllPath = relBuf;
    }
    else
    {
        std::wstring sideCandidate = (exeDir / g_Profile->dllFilename).wstring();
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
        (std::filesystem::path(g_Profile->dllFilename).stem().wstring() + L"_copy.dll")).wstring();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Debug auto-attach  (Debug builds only)
// ─────────────────────────────────────────────────────────────────────────────

#ifdef _DEBUG
void AttachDebugger(DWORD pid, DWORD timeoutMs)
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
//  Shared memory
// ─────────────────────────────────────────────────────────────────────────────

bool InitSharedMemory()
{
    g_hLogEvent = CreateEventW(NULL, FALSE, FALSE, g_Profile->eventLogReady);
    g_hCmdEvent = CreateEventW(NULL, FALSE, FALSE, g_Profile->eventCmdReady);
    g_hShutdownEvent = CreateEventW(NULL, TRUE, FALSE, g_Profile->eventShutdown);
    g_hDllReadyEvent = CreateEventW(NULL, TRUE, FALSE, g_Profile->eventDllReady); // manual reset

    if (!g_hLogEvent || !g_hCmdEvent || !g_hShutdownEvent || !g_hDllReadyEvent)
    {
        Log("Failed to create events (err " + std::to_string(GetLastError()) + ")");
        return false;
    }

    g_hLogMapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(LogBuffer), g_Profile->shmemLogs);
    if (!g_hLogMapping) { Log("Failed to create log mapping"); return false; }
    bool logNew = (GetLastError() != ERROR_ALREADY_EXISTS);
    g_pLogBuffer = static_cast<LogBuffer*>(MapViewOfFile(g_hLogMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_pLogBuffer) { Log("Failed to map log buffer"); return false; }
    if (logNew) new (g_pLogBuffer) LogBuffer();

    g_hCmdMapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(CommandBuffer), g_Profile->shmemCmd);
    if (!g_hCmdMapping) { Log("Failed to create command mapping"); return false; }
    bool cmdNew = (GetLastError() != ERROR_ALREADY_EXISTS);
    g_pCmdBuffer = static_cast<CommandBuffer*>(MapViewOfFile(g_hCmdMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_pCmdBuffer) { Log("Failed to map command buffer"); return false; }
    if (cmdNew) new (g_pCmdBuffer) CommandBuffer();

    g_hRespEvent = CreateEventW(NULL, TRUE, FALSE, g_Profile->eventRespReady);
    if (!g_hRespEvent) { Log("Failed to create response event"); return false; }
    g_hRespMapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(ResponseBuffer), g_Profile->shmemResponse);
    if (!g_hRespMapping) { Log("Failed to create response mapping"); return false; }
    bool respNew = (GetLastError() != ERROR_ALREADY_EXISTS);
    g_pRespBuffer = static_cast<ResponseBuffer*>(MapViewOfFile(g_hRespMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_pRespBuffer) { Log("Failed to map response buffer"); return false; }
    if (respNew) new (g_pRespBuffer) ResponseBuffer();

    g_hMetaMapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(MetaBuffer), g_Profile->shmemMeta);
    if (g_hMetaMapping)
    {
        bool metaNew = (GetLastError() != ERROR_ALREADY_EXISTS);
        g_pMetaBuffer = static_cast<MetaBuffer*>(MapViewOfFile(g_hMetaMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (g_pMetaBuffer)
        {
            if (metaNew) new (g_pMetaBuffer) MetaBuffer();
            g_pMetaBuffer->cliHwnd = reinterpret_cast<uintptr_t>(GetConsoleWindow());
        }
    }

    return true;
}

void CleanupSharedMemory()
{
    if (g_pLogBuffer)  UnmapViewOfFile(g_pLogBuffer);
    if (g_pCmdBuffer)  UnmapViewOfFile(g_pCmdBuffer);
    if (g_pRespBuffer) UnmapViewOfFile(g_pRespBuffer);
    if (g_pMetaBuffer) UnmapViewOfFile(g_pMetaBuffer);

    if (g_hLogMapping)    CloseHandle(g_hLogMapping);
    if (g_hCmdMapping)    CloseHandle(g_hCmdMapping);
    if (g_hRespMapping)   CloseHandle(g_hRespMapping);
    if (g_hMetaMapping)   CloseHandle(g_hMetaMapping);
    if (g_hLogEvent)      CloseHandle(g_hLogEvent);
    if (g_hCmdEvent)      CloseHandle(g_hCmdEvent);
    if (g_hShutdownEvent) CloseHandle(g_hShutdownEvent);
    if (g_hRespEvent)     CloseHandle(g_hRespEvent);
    if (g_hDllReadyEvent) CloseHandle(g_hDllReadyEvent);

    g_pLogBuffer  = nullptr;
    g_pCmdBuffer  = nullptr;
    g_pRespBuffer = nullptr;
    g_pMetaBuffer = nullptr;

    g_hLogMapping    = NULL;
    g_hCmdMapping    = NULL;
    g_hRespMapping   = NULL;
    g_hMetaMapping   = NULL;
    g_hLogEvent      = NULL;
    g_hCmdEvent      = NULL;
    g_hShutdownEvent = NULL;
    g_hRespEvent     = NULL;
    g_hDllReadyEvent = NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Inject / Unload
// ─────────────────────────────────────────────────────────────────────────────

bool InjectDLL()
{
    std::lock_guard<std::mutex> lock(g_InjectionMutex);
    if (DllActive()) return true;
    if (!ResolveDllPaths()) return false;

    DWORD pid = GetProcId(g_Profile->targetProcess);
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
                        // Snapshot the copy DLL's mtime as the injection baseline so
                        // HotReloadThread doesn't immediately see a "newer" source and
                        // spuriously reload.  CopyFileW preserves the source mtime, so
                        // this is equivalent to what the normal inject path records.
                        WIN32_FILE_ATTRIBUTE_DATA copyInfo{};
                        if (GetFileAttributesExW(g_CopyDllPath.c_str(), GetFileExInfoStandard, &copyInfo))
                            g_LastInjectedTime = copyInfo.ftLastWriteTime;
                        // The DLL's ready event was already fired and reset by the
                        // previous CLI session.  Re-signal it so DllReadyThread
                        // wakes up and auto-loads the command list.
                        if (g_hDllReadyEvent) SetEvent(g_hDllReadyEvent);
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
        DWORD pidCheck = GetProcId(g_Profile->targetProcess);
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

    pid = GetProcId(g_Profile->targetProcess);
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

void UnloadDLL(bool suppress)
{
    std::lock_guard<std::mutex> lock(g_InjectionMutex);
    if (!DllActive()) { Log("UnloadDLL: not active, skipping."); return; }
    if (!g_hProcess) { Log("UnloadDLL: no process handle.");   return; }

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

        HANDLE hDone = OpenEventW(SYNCHRONIZE, FALSE, g_Profile->eventShutdownDone);
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

    DWORD pid = GetProcId(g_Profile->targetProcess);
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
//  Dumper7
// ─────────────────────────────────────────────────────────────────────────────

bool LoadDumper7()
{
    std::lock_guard<std::mutex> lock(g_InjectionMutex);

    DWORD pid = GetProcId(g_Profile->targetProcess);
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

bool UnloadDumper7()
{
    Log("Dumper7: external unload disabled — press F6 in-game to unload.");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Background threads
// ─────────────────────────────────────────────────────────────────────────────

void DllLogThread(LogBuffer* pLog, HANDLE hLogEvent)
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

// Sleep up to `ms`, but return early (within ~50ms) once g_Running clears, so a
// profile switch — which join()s these poll threads — isn't stalled for a full
// poll interval. Identical total delay to Sleep(ms) during normal operation.
static void InterruptibleSleep(DWORD ms)
{
    constexpr DWORD step = 50;
    for (DWORD elapsed = 0; elapsed < ms && g_Running.load(std::memory_order_relaxed); elapsed += step)
    {
        const DWORD chunk = (ms - elapsed < step) ? (ms - elapsed) : step;
        Sleep(chunk);
    }
}

void HotReloadThread()
{
    while (g_Running)
    {
        InterruptibleSleep(kWatchPollMs);
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

void ProcessWatcherThread()
{
    while (g_Running)
    {
        InterruptibleSleep(kWatchPollMs);

        InjectionState state = g_InjState.load();

        if (state == InjectionState::Suppressed) continue;

        if (state == InjectionState::Active)
        {
            if (!IsProcessAlive(g_hProcess))
            {
                Log("ProcessWatcher: target process exited unexpectedly — waiting for relaunch...");
                if (g_hProcess) { CloseHandle(g_hProcess); g_hProcess = NULL; }
                g_InjState.store(InjectionState::Watching);
                kWatchPollMs = 50;
                Log("NewDelay: " + std::to_string(kWatchPollMs) + " ms");
            }
            continue;
        }

        DWORD pid = GetProcId(g_Profile->targetProcess);
        if (!pid) continue;

        Log("ProcessWatcher: target process found (PID " + std::to_string(pid) + "), injecting...");
        if (InjectDLL())
        {
            kWatchPollMs = 1000;
            Log("ProcessWatcher: auto-inject succeeded.");
            Log("NewDelay: " + std::to_string(kWatchPollMs) + " ms");
        }
        else
            Log("ProcessWatcher: inject failed, will retry...");
    }
}
