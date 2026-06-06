#pragma once
// Lib_OverlayConsole.h — "the CLI, in-game".
//
// Mirrors the DLL's log stream into an in-overlay log pane, and exposes the
// CommandHandler as Console + Commands panels (run commands by typing or by
// clicking). Commands are dispatched on the game thread via EnqueueOnce, reusing
// CommandHandler::Dispatch — the same entry point the CLI drives over IPC.

#include <spdlog/spdlog.h>   // spdlog::sink_ptr

class CommandHandler;

namespace OverlayConsole
{
    // spdlog sink that feeds the in-game log pane. Add it to the DLL's logger at
    // creation (Main.cpp). Lazily created; thread-safe.
    spdlog::sink_ptr GetSink();

    // Register the Console + Commands panels on the overlay, bound to `handler`.
    // Call once after the overlay exists and the handler is populated.
    void Init(CommandHandler* handler);
}
