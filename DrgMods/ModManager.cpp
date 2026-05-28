#define USEOWNIMPL 0
#include "ModManager.h"
#include "Commands.h"
#include "Library.h"
#if USEOWNIMPL
#include "UnrealCoreTypes.h"
#endif

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
    static void WaitForShutdown() {
        return;
        //while (GameHooks::ProcessEventHook::Get().IsInstalled())
        //    Sleep(1);
    }

    static bool TestAllocator()
    {
        constexpr uint32 size = 1024;
        constexpr const char* testStr = "Hello from GMalloc!\0";

        SDK::UnrealAllocator* allocator = SDK::UnrealAllocator::Get(true);
        if (!allocator)
        {
            error("[ModManager] UnrealAllocator is null");
            return false;
        }
        info("[ModManager] Found GMalloc at {:p} ({})",
            static_cast<const void*>(allocator),
            FString(allocator->GetDescriptiveName()).ToString());
        info("[ModManager] Testing GMalloc...");

        void* mem = SDK::FMemory::Malloc(size);
        if (!mem)
        {
            error("[ModManager] GMalloc allocation failed.");
            return false;
        }

        SDK::FMemory::Memcpy(mem, testStr, 21);

        uint64 actualSize = 0;
        if (!SDK::FMemory::GetAllocSize(mem, actualSize) || actualSize < size)
        {
            error("[ModManager] Allocation size validation failed (got {}).", actualSize);
            SDK::FMemory::Free(mem);
            return false;
        }

        info("[ModManager] GMalloc allocation successful.");
        info("[ModManager] Allocated size: {} bytes", actualSize);
        info("[ModManager] Test allocation address: {:p}", mem);
        info("[ModManager] Is Allocator Internally Thread Safe? {}", allocator->IsInternallyThreadSafe() ? "Yes" : "No");
        info("[ModManager] Test allocation content: {}", std::string(static_cast<char*>(mem), 21));
        info("[ModManager] Test allocation content validity: {}", std::string(static_cast<char*>(mem)) == testStr ? "Valid" : "Invalid");
        info("[ModManager] GMalloc test passed.");
        SDK::FMemory::Free(mem);
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
                GameHooks::ProcessEventHook::Get().SetOnUninstalled([this]() { LoadMods(); });
                UnloadMods();
            }, "Reload all mods");
        cmdHandler.Register("listcmds", [this](const CommandContext& ctx) {
            SendCommandList(ctx, cmdHandler);
            }, "List all registered commands (populates CLI autocomplete)");
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
    if (!TestAllocator()) 
    {
        error("[ModManager] Issue with allocator instance (how did we not crash yet?)");
        return;
    }
    if (!GameHooks::InstallProcessEventHook())
    {
        error("[ModManager] ProcessEvent hook failed to install.");
        return;
    }
    if (!GameHooks::InstallEngineTickHook(VTableLayout::UEngine::Tick))
        error("[ModManager] EngineTick hook failed to install (slot {}).", VTableLayout::UEngine::Tick);
    GameHooks::ProcessEventHook::Get().Enqueue([this]() { LoadModsGameThread(); });
}


void ModManager::LoadModsGameThread()
{
    info("----------------------------------------");
    info("[ModManager] Starting initialization...");
    if (!GameHooks::InstallProcessEventHook()) [[unlikely]]
    {
        error("[ModManager] ProcessEvent hook failed to install.");
        return;
    }
    if (!StartupInfo()) [[unlikely]] return;
    InitDefaultCallbacks();
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
    TickSystem::Reset();
    ResetCallbackHandles();
    VarSystem::Clear();
    JsonHook::Teardown(); // restore ExecFunction/flags before DLL pages are freed
    GameHooks::EngineTickHook::Get().RequestUninstall();
    GameHooks::ProcessEventHook::Get().RequestUninstall();
    WaitForShutdown();

    info("----------------------------------------");
}

void ModManager::Update(int DeltaTimeMs)
{
    GameHooks::ProcessEventHook::Get().Enqueue([this, DeltaTimeMs]()
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