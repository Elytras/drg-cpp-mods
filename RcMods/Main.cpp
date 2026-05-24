#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include "Common.h"
#include "Lib_KeyBindings.h"
#include "ModManager.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────────────────────────────────────

std::atomic<bool> g_KeepRunning{ true };
HMODULE           g_hMe = NULL;

// Events
HANDLE g_hLogEvent = NULL;
HANDLE g_hCmdEvent = NULL;
HANDLE g_hShutdownEvent = NULL;
HANDLE g_hRespEvent = NULL;
HANDLE g_hDllReadyEvent = NULL;

// Mappings
HANDLE g_hLogMapping  = NULL;
HANDLE g_hCmdMapping  = NULL;
HANDLE g_hRespMapping = NULL;
HANDLE g_hMetaMapping = NULL;

// Buffers
LogBuffer*      g_pLogBuffer  = nullptr;
CommandBuffer*  g_pCmdBuffer  = nullptr;
ResponseBuffer* g_pRespBuffer = nullptr;
MetaBuffer*     g_pMetaBuffer = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
//  Response channel
// ─────────────────────────────────────────────────────────────────────────────

void SendResponse(uint32_t cmdSeq, const std::string& msg)
{
    if (!g_pRespBuffer || !g_hRespEvent) return;

    constexpr DWORD MAX_WAIT_MS = 1000;
    DWORD deadline = GetTickCount() + MAX_WAIT_MS;
    while (g_pRespBuffer->ready.load(std::memory_order_acquire))
    {
        if (GetTickCount() > deadline) return;
        Sleep(5);
    }

    g_pRespBuffer->type = ResponseType::Text;
    strncpy_s(g_pRespBuffer->data.text, RESPONSE_BUFFER_SIZE, msg.c_str(), RESPONSE_BUFFER_SIZE - 1);
    g_pRespBuffer->seq.store(cmdSeq, std::memory_order_release);
    g_pRespBuffer->ready.store(true, std::memory_order_release);
    SetEvent(g_hRespEvent);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Custom spdlog sink
// ─────────────────────────────────────────────────────────────────────────────

template<typename Mutex>
class shmem_sink : public spdlog::sinks::base_sink<Mutex>
{
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        if (!g_pLogBuffer || !g_hLogEvent) return;

        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);

        for (size_t i = 0; i < formatted.size(); ++i)
            WriteChar(formatted.data()[i]);

        if (formatted.size() == 0 || formatted.data()[formatted.size() - 1] != '\n')
            WriteChar('\n');

        SetEvent(g_hLogEvent);
    }

    void flush_() override {}

private:
    void WriteChar(char c)
    {
        uint32_t pos = g_pLogBuffer->writePos.load(std::memory_order_relaxed);
        g_pLogBuffer->data[pos % LOG_BUFFER_SIZE] = c;
        g_pLogBuffer->writePos.store(pos + 1, std::memory_order_release);
    }
};

using shmem_sink_mt = shmem_sink<std::mutex>;

// ─────────────────────────────────────────────────────────────────────────────
//  Shared memory init / cleanup
// ─────────────────────────────────────────────────────────────────────────────

bool InitSharedMemory()
{
    g_hLogEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, EVENT_LOG_READY);
    g_hCmdEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, EVENT_CMD_READY);
    g_hShutdownEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, EVENT_SHUTDOWN);
    g_hDllReadyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, EVENT_DLL_READY);

    if (!g_hLogEvent || !g_hCmdEvent || !g_hShutdownEvent)
        return false;

    g_hLogMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SHMEM_LOGS);
    if (!g_hLogMapping) return false;
    g_pLogBuffer = static_cast<LogBuffer*>(MapViewOfFile(g_hLogMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_pLogBuffer) return false;

    g_hCmdMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SHMEM_CMD);
    if (!g_hCmdMapping) return false;
    g_pCmdBuffer = static_cast<CommandBuffer*>(MapViewOfFile(g_hCmdMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_pCmdBuffer) return false;

    // Response channel — optional, don't hard-fail if injector is old build
    g_hRespMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SHMEM_RESPONSE);
    if (g_hRespMapping)
    {
        g_pRespBuffer = static_cast<ResponseBuffer*>(
            MapViewOfFile(g_hRespMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        g_hRespEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, EVENT_RESP_READY);
        if (g_pRespBuffer)
            g_pRespBuffer->ready.store(false, std::memory_order_release);
    }

    // Meta buffer — optional; carries CLI HWND for focus-aware keybindings
    g_hMetaMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, SHMEM_META);
    if (g_hMetaMapping)
        g_pMetaBuffer = static_cast<MetaBuffer*>(MapViewOfFile(g_hMetaMapping, FILE_MAP_READ, 0, 0, 0));

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
//  Worker thread
// ─────────────────────────────────────────────────────────────────────────────

void WorkerThread()
{
    HANDLE hDone = CreateEventW(NULL, TRUE, FALSE, EVENT_SHUTDOWN_DONE);

    if (!InitSharedMemory())
        return;

    try
    {
        auto sink = std::make_shared<shmem_sink_mt>();
        sink->set_pattern("%v");
        sink->set_level(spdlog::level::trace);

        auto logger = std::make_shared<spdlog::logger>("rc", sink);
        logger->set_level(spdlog::level::trace);

        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::trace);
        spdlog::flush_on(spdlog::level::trace);
        spdlog::info("RC DLL initialized{}",
            g_pRespBuffer ? " (response channel active)" : " (no response channel)");
    }
    catch (...) { return; }

    if (g_pMetaBuffer)
        KeyBindings::SetCLIWindow(g_pMetaBuffer->cliHwnd);
    KeyBindings::Init();

    ModManager manager;
    manager.LoadMods();

    HANDLE waitHandles[] = { g_hShutdownEvent, g_hCmdEvent };
    constexpr DWORD kWaitMs = 100;

    while (g_KeepRunning)
    {
        DWORD result = WaitForMultipleObjects(2, waitHandles, FALSE, kWaitMs);

        switch (result)
        {
        case WAIT_OBJECT_0:
            spdlog::info("Shutdown requested");
            g_KeepRunning = false;
            break;

        case WAIT_OBJECT_0 + 1:
            if (g_pCmdBuffer->hasCommand.load(std::memory_order_acquire))
            {
                std::string cmd(g_pCmdBuffer->command);
                uint32_t    seq = g_pCmdBuffer->seq.load(std::memory_order_acquire);
                g_pCmdBuffer->hasCommand.store(false, std::memory_order_release);
                manager.Message(cmd, seq);
            }
            break;

        case WAIT_TIMEOUT:
            manager.Update(kWaitMs);
            break;

        default:
            spdlog::error("WaitForMultipleObjects failed: {}", GetLastError());
            g_KeepRunning = false;
            break;
        }
    }

    manager.UnloadMods();
    KeyBindings::Shutdown();
    spdlog::info("RC DLL shutdown complete");
    spdlog::shutdown();
    CleanupSharedMemory();

    if (hDone)
    {
        SetEvent(hDone);
        CloseHandle(hDone);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  DllMain
// ─────────────────────────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hMe = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0,
            [](LPVOID) -> DWORD { WorkerThread(); return 0; },
            nullptr, 0, nullptr);
    }
    return TRUE;
}
