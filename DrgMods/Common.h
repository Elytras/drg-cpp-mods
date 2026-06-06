#pragma once
// Common.h (DRG) — Per-game string constants only; IPC layouts are in
// SharedLib/core/IpcProtocol.h.

#include "IpcProtocol.h"

//-----------------------------------------------------------------------------
// Process / DLL identity
//-----------------------------------------------------------------------------
constexpr const wchar_t* TARGET_PROCESS = L"FSD-Win64-Shipping.exe";
constexpr const wchar_t* DLL_FILENAME = L"DrgMods.dll";
constexpr const char* TARGET_PROCESS_NARROW = "FSD-Win64-Shipping.exe";

//-----------------------------------------------------------------------------
// Shared memory / event names
//-----------------------------------------------------------------------------
constexpr const wchar_t* SHMEM_LOGS     = L"Local\\DRG_Logs";
constexpr const wchar_t* SHMEM_INJLOG   = L"Local\\DRG_InjLog";
constexpr const wchar_t* SHMEM_CMD      = L"Local\\DRG_Commands";
constexpr const wchar_t* SHMEM_RESPONSE = L"Local\\DRG_Response";
constexpr const wchar_t* SHMEM_META     = L"Local\\DRG_Meta";

constexpr const wchar_t* EVENT_LOG_READY = L"Local\\DRG_LogReady";
constexpr const wchar_t* EVENT_INJLOG_READY = L"Local\\DRG_InjLogReady";
constexpr const wchar_t* EVENT_CMD_READY = L"Local\\DRG_CmdReady";
constexpr const wchar_t* EVENT_RESP_READY = L"Local\\DRG_ResponseReady";
constexpr const wchar_t* EVENT_SHUTDOWN = L"Local\\DRG_Shutdown";
constexpr const wchar_t* EVENT_SHUTDOWN_DONE = L"Local\\DRG_ShutdownDone";
constexpr const wchar_t* EVENT_DLL_READY = L"Local\\DRG_DllReady";

//-----------------------------------------------------------------------------
// Output Locations
//-----------------------------------------------------------------------------

constexpr const char* OUTPUT_DIR = R"(D:\Repos\CppDrg\Drgmods\DrgMods\ModOutput\)";
