#pragma once

//-----------------------------------------------------------------------------
// Windows
//-----------------------------------------------------------------------------
#ifndef EXTRA_LEAN
#define EXTRA_LEAN
#endif

#include <Windows.h>

//-----------------------------------------------------------------------------
// STL
//-----------------------------------------------------------------------------
#include <atomic>

//-----------------------------------------------------------------------------
// Size helpers
//-----------------------------------------------------------------------------
constexpr uint64_t KB(uint64_t x) { return x * 1024; }
constexpr uint64_t MB(uint64_t x) { return KB(x) * 1024; }
constexpr uint64_t GB(uint64_t x) { return MB(x) * 1024; }
constexpr uint64_t TB(uint64_t x) { return GB(x) * 1024; }

//-----------------------------------------------------------------------------
// Process / DLL identity
//-----------------------------------------------------------------------------
constexpr const wchar_t* TARGET_PROCESS = L"FSD-Win64-Shipping.exe";
constexpr const wchar_t* DLL_FILENAME = L"DrgMods.dll";
constexpr const char* TARGET_PROCESS_NARROW = "FSD-Win64-Shipping.exe";

//-----------------------------------------------------------------------------
// Shared memory / event names
//-----------------------------------------------------------------------------
constexpr const wchar_t* SHMEM_LOGS = L"Local\\DRG_Logs";
constexpr const wchar_t* SHMEM_INJLOG = L"Local\\DRG_InjLog";
constexpr const wchar_t* SHMEM_CMD = L"Local\\DRG_Commands";
constexpr const wchar_t* SHMEM_RESPONSE = L"Local\\DRG_Response";

constexpr const wchar_t* EVENT_LOG_READY = L"Local\\DRG_LogReady";
constexpr const wchar_t* EVENT_INJLOG_READY = L"Local\\DRG_InjLogReady";
constexpr const wchar_t* EVENT_CMD_READY = L"Local\\DRG_CmdReady";
constexpr const wchar_t* EVENT_RESP_READY = L"Local\\DRG_ResponseReady";
constexpr const wchar_t* EVENT_SHUTDOWN = L"Local\\DRG_Shutdown";
constexpr const wchar_t* EVENT_SHUTDOWN_DONE = L"Local\\DRG_ShutdownDone";

//-----------------------------------------------------------------------------
// Output Locations
//-----------------------------------------------------------------------------

constexpr const char* OUTPUT_DIR = R"(D:\Repos\CppDrg\Drgmods\DrgMods\ModOutput\)";

//-----------------------------------------------------------------------------
// Capacity constants
//-----------------------------------------------------------------------------
constexpr size_t MAX_FUNC_NAME = 128;
constexpr size_t MAX_PARAM_STR = 256;
constexpr size_t MAX_SCAN_RESULT = 1024;

constexpr int MAX_CMD_COUNT = 64;
constexpr int MAX_CMD_NAME = 64;
constexpr int MAX_CMD_DESC = 128;

constexpr size_t LOG_BUFFER_SIZE = KB(64);   // 64 KB
constexpr size_t CMD_BUFFER_SIZE = 4097;
constexpr size_t RESPONSE_BUFFER_SIZE = KB(512);  // 512 KB

//-----------------------------------------------------------------------------
// Scan structures
//-----------------------------------------------------------------------------
struct ScannedFunctionInfo
{
    char name[MAX_FUNC_NAME];    // e.g. "Server_SetHeadLight"
    char owner[MAX_FUNC_NAME];   // e.g. "BP_GunnerCharacter_C"
    char params[MAX_PARAM_STR];  // comma-separated, empty if none
};

struct CmdEntry
{
    char name[MAX_CMD_NAME];
    char desc[MAX_CMD_DESC];   // the usage string from Register()
};

struct ScanResponse
{
    uint32_t           count;
    ScannedFunctionInfo funcs[MAX_SCAN_RESULT];
};

struct CommandsResponse
{
    uint32_t count;
    CmdEntry cmds[MAX_CMD_COUNT];
};
//-----------------------------------------------------------------------------
// Shared memory layouts
//-----------------------------------------------------------------------------
struct LogBuffer
{
    std::atomic<uint32_t> writePos{ 0 };
    std::atomic<uint32_t> readPos{ 0 };
    char data[LOG_BUFFER_SIZE];
};

struct CommandBuffer
{
    char                  command[CMD_BUFFER_SIZE];
    std::atomic<bool>     hasCommand{ false };
    std::atomic<uint32_t> seq{ 0 };
};
static_assert(sizeof(CommandBuffer) % 8 == 0, "CommandBuffer size must be a multiple of 8");

enum class ResponseType : uint8_t
{
    Text = 0,       // data is a null-terminated string
    Scan = 1,       // data is a ScanResponse
    Commands = 2,   // data is a list of available commands
};

struct ResponseBuffer
{
    std::atomic<uint32_t> seq{ 0 };
    std::atomic<bool>     ready{ false };
    ResponseType          type{ ResponseType::Text };
    union {
        char         text[RESPONSE_BUFFER_SIZE];
        ScanResponse scan;
        CommandsResponse commands;
    } data;
};

//-----------------------------------------------------------------------------
// Command protocol
//-----------------------------------------------------------------------------
enum class CommandType : uint32_t
{
    None = 0,
    Load,
    Unload,
    Reload,
    Custom,
};

//-----------------------------------------------------------------------------
// Assume-Assert
//-----------------------------------------------------------------------------

#if defined(NDEBUG)

#define ASSUME_ASSERT(expr) \
      __assume(static_cast<bool>(expr))

#else
#include <cassert>
#define ASSUME_ASSERT(expr) \
      assert(expr)

#endif