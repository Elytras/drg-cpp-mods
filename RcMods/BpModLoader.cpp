#include "BpModLoader.h"
#include "Library.h"
#include "SDK/SDK/AssetRegistry_classes.hpp"
// AssetRegistry_functions.cpp is compiled as its own TU (via RcMods.vcxproj);
// including just the header here is enough to call the generated functions.
#include <unordered_map>

using namespace SDK;

namespace BpModLoader
{
    // ── Constants ─────────────────────────────────────────────────────────────

    static constexpr const wchar_t* kModPath    = L"/Game/_Mods";
    static constexpr const char*    kNamePrefix  = "InitMod_";

    // ── Module state ──────────────────────────────────────────────────────────

    static UWorld*                   s_LastWorld  = nullptr;
    static GameHooks::CallbackHandle s_TickHandle = 0;

    static WorldCallbackHandle                                         s_NextHandle    = 1;
    static std::unordered_map<WorldCallbackHandle, WorldChangedFn>     s_WorldCallbacks;

    // ── Helpers ───────────────────────────────────────────────────────────────

    // Construct an identity FTransform: no translation, identity rotation,
    // unit scale.  FQuat is double-precision (X,Y,Z,W); FVector is double (X,Y,Z).
    static FTransform IdentityTransform()
    {
        FTransform xf{};
        xf.Rotation.W    = 1.0;           // identity quaternion
        xf.Scale3D.X     = 1.0;
        xf.Scale3D.Y     = 1.0;
        xf.Scale3D.Z     = 1.0;
        return xf;
    }

    // ── Core logic ────────────────────────────────────────────────────────────

    static void SpawnModActors()
    {
        UWorld* world = UWorld::GetWorld();
        if (!world) return;
        static FName s_ModPathFName(kModPath);

        // TArray<FName> on x64: { FName* Data; int32 Num; int32 Max; } = 16 bytes
        struct FakeTArray { FName* Data; int32_t Num; int32_t Max; };
        static_assert(sizeof(FakeTArray) == sizeof(TArray<FName>),
                      "TArray layout mismatch — check UnrealContainers.hpp");

        FakeTArray pathArr{ &s_ModPathFName, 1, 1 };

        FARFilter filter{};
        // PackagePaths is at FARFilter+0x0010 (see CoreUObject_structs.hpp).
        memcpy(&filter.PackagePaths, &pathArr, sizeof(pathArr));
        filter.bRecursivePaths = true;

        TArray<FAssetData> assets{};
        UAssetRegistryHelpers::GetBlueprintAssets(filter, &assets);

        if (assets.Num() == 0)
        {
            info("[BpModLoader] No blueprint assets found under '{}'.",
                 StringLib::ToNarrow(kModPath));
            return;
        }

        const FTransform spawnXf = IdentityTransform();
        int spawned = 0;

        for (int i = 0; i < assets.Num(); ++i)
        {
            const FAssetData& ad       = assets[i];
            std::string       assetName = ad.AssetName.ToString();

            // Only care about InitMod_* blueprints.
            if (assetName.find(kNamePrefix) != 0) continue;

            // Force the package into memory.  In a packaged game, GetAsset()
            // loads the compiled UBlueprintGeneratedClass and may return it
            // directly as a UObject*.
            UObject* loaded = UAssetRegistryHelpers::GetAsset(ad);
            if (!loaded)
            {
                warn("[BpModLoader] Failed to load asset '{}'.", assetName);
                continue;
            }

            // Resolve the spawnable UClass.
            // Case A: GetAsset() returned the class itself (packaged game).
            // Case B: Loading triggered registration of the _C generated class.
            UClass* cls = nullptr;
            if (loaded->IsA(UClass::StaticClass()))
            {
                cls = static_cast<UClass*>(loaded);
            }
            else
            {
                // Generated class carries a _C suffix in the short name.
                cls = BasicFilesImpleUtils::FindClassByName(assetName + "_C");
                if (!cls)
                    cls = BasicFilesImpleUtils::FindClassByName(assetName);
            }

            if (!cls)
            {
                warn("[BpModLoader] Could not find generated class for '{}'.",
                     assetName);
                continue;
            }

            if (!cls->IsA(EClassCastFlags::Actor))
            {
                warn("[BpModLoader] '{}' is not an AActor subclass — skipping.",
                     cls->GetName());
                continue;
            }

            // Spawn: deferred so the actor can initialise before FinishSpawning.
            AActor* actor = UGameplayStatics::BeginDeferredActorSpawnFromClass(
                world,
                TSubclassOf<AActor>(cls),
                spawnXf,
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
                /*Owner=*/nullptr,
                ESpawnActorScaleMethod::SelectDefaultAtRuntime);

            if (!actor)
            {
                warn("[BpModLoader] BeginDeferredActorSpawnFromClass returned null for '{}'.",
                     cls->GetName());
                continue;
            }

            UGameplayStatics::FinishSpawningActor(actor, spawnXf,
                ESpawnActorScaleMethod::SelectDefaultAtRuntime);

            info("[BpModLoader] Spawned '{}' (class '{}').",
                 assetName, cls->GetName());
            ++spawned;
        }

        info("[BpModLoader] Done — {} InitMod_ actor(s) spawned for world '{}'.",
             spawned, world->GetName());
    }

    // ── Public API ────────────────────────────────────────────────────────────

    WorldCallbackHandle OnWorldChanged(WorldChangedFn callback)
    {
        const WorldCallbackHandle h = s_NextHandle++;
        s_WorldCallbacks.emplace(h, std::move(callback));
        return h;
    }

    void RemoveOnWorldChanged(WorldCallbackHandle handle)
    {
        s_WorldCallbacks.erase(handle);
    }

    void Install()
    {
        if (s_TickHandle != 0) return;
        s_LastWorld = nullptr;  // force spawn on the very first tick

        s_TickHandle = GameHooks::OnEngineTick(
            [](UEngine*, float, bool)
            {
                // Guard against the one stale tick that can fire after
                // RemoveCallback() — it dispatches from the pre-removal
                // snapshot but Uninstall() has already zeroed s_TickHandle.
                if (s_TickHandle == 0) return;

                UWorld* world = UWorld::GetWorld();

                // No world yet, or same world as last time.
                if (!world || world == s_LastWorld) return;

                // Wait until the local controller is present — ensures the
                // level has finished its initial setup before we spawn.
                if (!IsValidOf<APlayerController>(GetLocalController())) return;

                info("[BpModLoader] Level transition → world '{}', spawning InitMod_ actors.",
                     world->GetName());
                s_LastWorld = world;
                
                // Doesnt't work right now so commented out for now to not pollute logs
                //SpawnModActors();

                for (auto& [h, fn] : s_WorldCallbacks)
                    fn(world);
            },
            GameHooks::ExecutionTiming::After);

        info("[BpModLoader] Installed — watching for level transitions.");
    }

    void Uninstall()
    {
        if (s_TickHandle == 0) return;
        GameHooks::EngineTickHook::Get().RemoveCallback(s_TickHandle);
        s_TickHandle = 0;
        s_LastWorld  = nullptr;
        s_WorldCallbacks.clear();
        info("[BpModLoader] Uninstalled.");
    }

} // namespace BpModLoader
