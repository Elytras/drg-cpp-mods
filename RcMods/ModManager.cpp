#include "ModManager.h"
#include "Commands.h"
#include "Library.h"
#include "BpModLoader.h"
#include "Lib_NetLogConfig.h"

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
    static void WaitForShutdown() {
        return;
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
    // EngineTick is the outermost hook — install it first so its task queue
    // is live before we enqueue anything.  ProcessEvent (inner, high-frequency)
    // is installed later from within the first tick.
    if (!GameHooks::InstallEngineTickHook(VTableLayout::UEngine::Tick))
    {
        error("[ModManager] EngineTick hook failed to install (slot {}).",
              VTableLayout::UEngine::Tick);
        return;
    }
    GameHooks::EngineTickHook::Get().Enqueue([this]() { LoadModsGameThread(); });
}


void ModManager::LoadModsGameThread()
{
    info("----------------------------------------");
    info("[ModManager] Starting initialization...");
    // ProcessEvent is the inner hook — install it here, on the game thread,
    // after the tick hook that bootstrapped us is already live.
    if (!GameHooks::InstallProcessEventHook()) [[unlikely]]
    {
        error("[ModManager] ProcessEvent hook failed to install.");
        return;
    }
    if (!StartupInfo()) [[unlikely]] return;
    BpModLoader::Install();
    InitDefaultCallbacks();
    RunConfig(cmdHandler);
    if constexpr (UseThreads) {
        shouldStop.store(false);
        modThread = std::thread(&ModManager::ModThreadWorker, this);
    }
    info("[ModManager] Initialization complete.");
    info("----------------------------------------");
    extern HANDLE g_hDllReadyEvent;
    if (g_hDllReadyEvent) SetEvent(g_hDllReadyEvent);
}

void ModManager::UnloadMods()
{
    info("----------------------------------------");
    info("[ModManager] Unloading...");
    BpModLoader::Uninstall();
    TickSystem::Reset();
    ResetCallbackHandles();
    VarSystem::Clear();
    GameHooks::ProcessEventHook::Get().RequestUninstall();  // inner first
    GameHooks::EngineTickHook::Get().RequestUninstall();    // outer last — owns the task queue
    WaitForShutdown();

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
