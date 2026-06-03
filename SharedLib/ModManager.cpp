// ModManager.cpp — shared mod-load orchestration for both games.
//
// Compiled once per consumer (DrgMods / RcMods) against that game's Library.h,
// like the other SharedLib *.cpp. The few per-game differences are expressed
// through the policy hooks declared in Commands.h (PreLoadCheck / OnModsLoaded /
// OnModsUnloading), implemented in each game's Commands.cpp.
#include "ModManager.h"
#include "Commands.h"
#include "Library.h"
#include "Lib_NetLogConfig.h"

// File-local SDK pollution: this TU does not use math wrappers (FVector etc.),
// so unqualified SDK type lookup here is unambiguous.
using namespace SDK;

static inline const constexpr bool UseThreads = false;

namespace Internal
{
    static bool StartupInfo() {
        // 1. Validate Engine
        auto* engine = SDK::UEngine::GetEngine();
        if (!engine) {
            error("[ModManager] UEngine is NULL. Aborting.");
            return false;
        }
        info("[ModManager] Engine OK.");

        auto* processEvent = InSDKUtils::GetVirtualFunction<void*>(engine, Offsets::ProcessEventIdx);
        info("[ModManager] Original ProcessEvent at: {:p}", (void*)processEvent);

        // 2. Validate World
        auto* world = SDK::UWorld::GetWorld();
        if (!world) {
            error("[ModManager] World doesn't exist yet");
            return true;
        }
        info("[ModManager] World OK.");

        // 3. Check Player Status (Non-fatal)
        if (auto* player = GetLocalPlayer()) {
            info("[ModManager] Local player found.");
        }
        else {
            info("[ModManager] Player doesn't exist yet.");
        }

        return true;
    }
}

using namespace ::Internal;

// =========================================================================
// Constructor
// =========================================================================
ModManager::ModManager()
{
    RegisterCommands(cmdHandler);
    cmdHandler.Register("retry", [this](const CommandContext&)
        {
            // EngineTick is the outermost hook and the last to be torn down —
            // wait for it to confirm clean shutdown before reinstalling.
            GameHooks::EngineTickHook::Get().SetOnUninstalled([this]() { LoadMods(); });
            UnloadMods();
        }, "Reload all mods");
    cmdHandler.Register("listcmds", [this](const CommandContext& ctx) {
        SendCommandList(ctx, cmdHandler);
        }, "List all registered commands (populates CLI autocomplete)");
    cmdHandler.Register("runcfg", [this](const CommandContext&) {
        RunConfig(cmdHandler);
        }, "Re-execute autorun entries from config.yaml");
}

// =========================================================================
// ModManager interface
// =========================================================================
void ModManager::Message(const std::string& msg, uint32_t seq)
{
    cmdHandler.Dispatch(msg, seq);
}

void ModManager::LoadMods()
{
    // Game-specific pre-flight check (DRG: GMalloc sanity test; RC: no-op).
    if (!PreLoadCheck())
    {
        error("[ModManager] Pre-load check failed. Aborting.");
        return;
    }

    // EngineTick is the outer hook — install it first so its task queue is live
    // before we enqueue anything. Installing it resolves (and waits for) the
    // engine UObject.
    if (!GameHooks::InstallEngineTickHook(VTableLayout::UEngine::Tick))
    {
        error("[ModManager] EngineTick hook failed to install (slot {}).",
              VTableLayout::UEngine::Tick);
        return;
    }

    // ProcessEvent (inner) lives at another slot in the SAME engine vtable, so it
    // can be installed right here now that the engine is resolved — no wait.
    if (!GameHooks::InstallProcessEventHook())
    {
        error("[ModManager] ProcessEvent hook failed to install.");
        return;
    }

    GameHooks::EngineTickHook::Get().Enqueue([this]() { LoadModsGameThread(); });
}

void ModManager::LoadModsGameThread()
{
    info("----------------------------------------");
    info("[ModManager] Starting initialization...");
    if (!StartupInfo()) [[unlikely]] return;
    OnModsLoaded();             // game-specific (RC: BpModLoader::Install)
    InitDefaultCallbacks();
    RunConfig(cmdHandler);
    if constexpr (UseThreads) {
        shouldStop.store(false);
        modThread = std::thread(&ModManager::ModThreadWorker, this);
    }
    info("[ModManager] Initialization complete.");
    info("----------------------------------------");

    // Signal the CLI that the DLL is ready to receive commands. The CLI's
    // ready-handler thread then sends `listcmds` to populate autocomplete —
    // we can't push the list ourselves because the CLI's REPL only consumes
    // responses tied to its own outgoing seq numbers.
    extern HANDLE g_hDllReadyEvent;
    if (g_hDllReadyEvent) SetEvent(g_hDllReadyEvent);
}

void ModManager::UnloadMods()
{
    info("----------------------------------------");
    info("[ModManager] Unloading...");
    OnModsUnloading();          // game-specific (DRG: JsonHook::Teardown; RC: BpModLoader::Uninstall)
    TickSystem::Reset();
    ResetCallbackHandles();
    GameHooks::ResetAllToggles();   // forget all HookToggle handles before the hooks tear down
    VarSystem::Clear();
    // SCO hook is opt-in (installed on demand). Tear it down explicitly so its
    // trampoline is gone before the DLL unloads and so a reload can re-install it.
    // Each hook now removes its own MinHook target; EasyHook releases MinHook once
    // the last hook is gone. Safe no-op if SCO was never installed.
    GameHooks::StaticConstructObjectHook::Get().RequestUninstall();
    GameHooks::ProcessEventHook::Get().RequestUninstall();  // inner first
    GameHooks::EngineTickHook::Get().RequestUninstall();    // outer last — owns the task queue

    info("----------------------------------------");
}

void ModManager::Update(int DeltaTimeMs)
{
    GameHooks::EngineTickHook::Get().Enqueue([this, DeltaTimeMs]()
        {
            UpdateGameThread(DeltaTimeMs);
        });
}

void ModManager::UpdateGameThread(int DeltaTimeMs)
{
    static auto lastTime = std::chrono::steady_clock::now();

    auto   now = std::chrono::steady_clock::now();
    long double ms = std::chrono::duration<long double, std::milli>(now - lastTime).count();
    lastTime = now;

    TickSystem::Dispatch(ms);
}

void ModManager::ModThreadWorker()
{
    info("[ModManager] Worker thread started.");
    while (!shouldStop.load())
    {
        try { Update(100); }
        catch (const std::exception& e) { error("[ModManager] Worker thread exception: {}", e.what()); }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    info("[ModManager] Worker thread stopped.");
}
