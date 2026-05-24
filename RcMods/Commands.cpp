#include "Commands.h"
#include "Library.h"

using namespace SDK;

// =========================================================================
// Tick system — stub for RogueCore
// =========================================================================

namespace TickSystem
{
    void Dispatch(long double /*actualDeltaMs*/) {}
    void Reset()                                 {}
}

// =========================================================================
// Callback handles
// =========================================================================

static GameHooks::CallbackHandle g_DefaultCallbackHandle{};

void ResetCallbackHandles()
{
    g_DefaultCallbackHandle = {};
}

// =========================================================================
// Default callbacks
// =========================================================================

void InitDefaultCallbacks()
{
    // Register default callbacks here.
}

// =========================================================================
// Command registration
// =========================================================================

void RegisterCommands(CommandHandler& handler)
{
    KeyBindings::RegisterCommands(handler);
}

// =========================================================================
// SendCommandList
// =========================================================================

void SendCommandList(const CommandContext& ctx, const CommandHandler& handler)
{
    auto& entries = handler.GetEntries();
    if (entries.empty()) return;

    extern ResponseBuffer* g_pRespBuffer;
    extern HANDLE          g_hRespEvent;
    if (!g_pRespBuffer || !g_hRespEvent) return;

    constexpr DWORD MAX_WAIT_MS = 1000;
    DWORD deadline = GetTickCount() + MAX_WAIT_MS;
    while (g_pRespBuffer->ready.load(std::memory_order_acquire))
    {
        if (GetTickCount() > deadline) return;
        Sleep(5);
    }

    auto& resp = g_pRespBuffer->data.commands;
    resp.count = 0;

    for (auto& [name, entry] : entries)
    {
        if (resp.count >= MAX_CMD_COUNT) break;
        auto& ce = resp.cmds[resp.count++];
        strncpy_s(ce.name, name.c_str(), MAX_CMD_NAME - 1);
        strncpy_s(ce.desc, entry.description.c_str(), MAX_CMD_DESC - 1);
    }

    g_pRespBuffer->type = ResponseType::Commands;
    g_pRespBuffer->seq.store(ctx.seq, std::memory_order_release);
    g_pRespBuffer->ready.store(true, std::memory_order_release);
    SetEvent(g_hRespEvent);
}
