#pragma once
// Common.h (RC) — Per-game string constants only; IPC layouts are in
// SharedLib/core/IpcProtocol.h.

#include "IpcProtocol.h"

//-----------------------------------------------------------------------------
// Process / DLL identity
//-----------------------------------------------------------------------------
constexpr const wchar_t* TARGET_PROCESS = L"RogueCore-Win64-Shipping.exe";
constexpr const wchar_t* DLL_FILENAME = L"RcMods.dll";
constexpr const char* TARGET_PROCESS_NARROW = "RogueCore-Win64-Shipping.exe";

//-----------------------------------------------------------------------------
// Shared memory / event names
//-----------------------------------------------------------------------------
constexpr const wchar_t* SHMEM_LOGS     = L"Local\\RC_Logs";
constexpr const wchar_t* SHMEM_INJLOG   = L"Local\\RC_InjLog";
constexpr const wchar_t* SHMEM_CMD      = L"Local\\RC_Commands";
constexpr const wchar_t* SHMEM_RESPONSE = L"Local\\RC_Response";
constexpr const wchar_t* SHMEM_META     = L"Local\\RC_Meta";

constexpr const wchar_t* EVENT_LOG_READY = L"Local\\RC_LogReady";
constexpr const wchar_t* EVENT_INJLOG_READY = L"Local\\RC_InjLogReady";
constexpr const wchar_t* EVENT_CMD_READY = L"Local\\RC_CmdReady";
constexpr const wchar_t* EVENT_RESP_READY = L"Local\\RC_ResponseReady";
constexpr const wchar_t* EVENT_SHUTDOWN = L"Local\\RC_Shutdown";
constexpr const wchar_t* EVENT_SHUTDOWN_DONE = L"Local\\RC_ShutdownDone";
constexpr const wchar_t* EVENT_DLL_READY = L"Local\\RC_DllReady";

//-----------------------------------------------------------------------------
// Output Locations
//-----------------------------------------------------------------------------

constexpr const char* OUTPUT_DIR = R"(D:\Repos\CppDrg\Drgmods\RcMods\ModOutput\)";
