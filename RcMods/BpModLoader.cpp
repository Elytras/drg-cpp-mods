#include "BpModLoader.h"
#include "Library.h"
#include "SDK/SDK/AssetRegistry_classes.hpp"
#include "SDK/SDK/AssetRegistry_parameters.hpp"
// AssetRegistry_functions.cpp is compiled as its own TU (via RcMods.vcxproj).
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace SDK;

namespace BpModLoader
{
    // ── Constants ─────────────────────────────────────────────────────────────

    static constexpr const wchar_t* kModPath    = L"/Game/_Mods";
    static constexpr const char*    kNamePrefix  = "InitMod_";

    // Flip to false once BpMod spawning works to quiet the per-class diagnostics.
    // Warnings/errors at each failure point stay on regardless of this flag.
    static constexpr bool           kVerbose     = true;

    // Late-loader tolerance: mod loaders that bootstrap themselves (e.g. via a
    // late widget-BP spawn) appear well after the level's local controller is
    // ready, so a one-shot scan on level load always misses them.  Instead we
    // keep rescanning for a window after every level transition and spawn each
    // InitMod_ class the moment it shows up (deduped so nothing spawns twice).
    static constexpr float          kRescanInterval = 2.0f;   // seconds between rescans
    static constexpr float          kRescanWindow   = 120.0f; // keep watching this long per world

    // ── Module state ──────────────────────────────────────────────────────────

    static UWorld*                   s_LastWorld  = nullptr;
    static GameHooks::CallbackHandle s_TickHandle = 0;

    static WorldCallbackHandle                                         s_NextHandle    = 1;
    static std::unordered_map<WorldCallbackHandle, WorldChangedFn>     s_WorldCallbacks;

    static std::unordered_set<UClass*>  s_spawned;          // classes spawned in the current world
    static float                        s_rescanAccum = 0.f;
    static float                        s_sinceWorld  = 0.f;

    // ── String helpers ──────────────────────────────────────────────────────────

    static bool StartsWith(const std::string& s, const char* p)
    {
        return s.rfind(p, 0) == 0;
    }
    static bool EndsWith(const std::string& s, const char* suf)
    {
        const std::string t = suf;
        return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
    }
    static bool ContainsCI(const std::string& s, const char* needle)
    {
        std::string a = s, b = needle;
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return a.find(b) != std::string::npos;
    }

    // Construct an identity FTransform: no translation, identity rotation,
    // unit scale.  FQuat is double-precision (X,Y,Z,W); FVector is double (X,Y,Z).
    static FTransform IdentityTransform()
    {
        FTransform xf{};
        xf.Rotation.W = 1.0;
        xf.Scale3D.X  = 1.0;
        xf.Scale3D.Y  = 1.0;
        xf.Scale3D.Z  = 1.0;
        return xf;
    }

    // ── Class resolution (all static helpers — reflection-safe) ──────────────────

    static UClass* ResolveAssetClass(const FAssetData& ad, const std::string& name)
    {
        // Canonical generated-class load:
        //   FAssetData → soft object path → soft class ref → LoadClassAsset_Blocking.
        // This explicitly loads (blocking) the UClass the asset represents — unlike
        // GetAsset (returns an ambiguous UObject) or GetClass (returns the metaclass,
        // e.g. UBlueprintGeneratedClass, which fails the IsSubclassOf(AActor) test).
        // This is the same path BP_ModHub uses (MakeSoftClassPath + load).
        FSoftObjectPath sop = UAssetRegistryHelpers::ToSoftObjectPath(ad);
        // FSoftClassPath is layout-identical to FSoftObjectPath (derived, no extra
        // members) — reinterpret to feed the class-ref converter.
        const FSoftClassPath& scp = *reinterpret_cast<const FSoftClassPath*>(&sop);
        TSoftClassPtr<UClass> sptr = UKismetSystemLibrary::Conv_SoftClassPathToSoftClassRef(scp);
        if (UClass* cls = UKismetSystemLibrary::LoadClassAsset_Blocking(sptr))
            return cls;

        // Fallback: name lookup (registry already lists the _C suffix).
        if (UClass* cls = BasicFilesImpleUtils::FindClassByName(name))        return cls;
        if (UClass* cls = BasicFilesImpleUtils::FindClassByName(name + "_C")) return cls;
        return nullptr;
    }

    // ── Discovery pass A: AssetRegistry (self-sufficient — loads packages) ───────
    //
    // The generated IAssetRegistry::GetAssetsByPath wrapper resolves the UFunction
    // via the LIVE object's class (AssetRegistryImpl), which does not own the
    // interface's functions — GetFunction returns null and the call crashes. We
    // instead pull the UFunction from the interface class (literally named
    // "AssetRegistry", which DOES own it) and ProcessEvent it on the live object.
    static void CollectFromRegistry(std::unordered_set<UClass*>& out, bool quiet)
    {
        UObject* registryObj = UAssetRegistryHelpers::GetAssetRegistry().GetObjectRef();
        if (!registryObj)
        {
            if (!quiet) warn("[BpModLoader] (registry) GetAssetRegistry returned null — skipping registry pass.");
            return;
        }

        UClass* iface = IAssetRegistry::StaticClass();
        if (!iface)
        {
            if (!quiet) warn("[BpModLoader] (registry) AssetRegistry interface class not found — skipping registry pass.");
            return;
        }

        // Force-index the mod path so mounted-but-unscanned pak content shows up.
        // A BP loader triggers this implicitly; doing it ourselves means discovery
        // does NOT depend on any other loader having run first.  Loud pass only
        // (once per world) — a synchronous scan can hitch.
        if (!quiet)
        {
            if (UFunction* scanFn = iface->GetFunction("AssetRegistry", "ScanPathsSynchronous"))
            {
                FString         modPath(kModPath);
                TArray<FString> inPaths(&modPath, 1, 1);
                Params::AssetRegistry_ScanPathsSynchronous sp{};
                sp.InPaths                    = inPaths;
                sp.bForceRescan               = false;
                sp.bIgnoreDenyListScanFilters = false;

                const auto sf = scanFn->FunctionFlags;
                scanFn->FunctionFlags |= 0x400;
                registryObj->ProcessEvent(scanFn, &sp);
                scanFn->FunctionFlags = sf;
                info("[BpModLoader] (registry) ScanPathsSynchronous('{}') done.", StringLib::ToNarrow(kModPath));
            }
            else warn("[BpModLoader] (registry) ScanPathsSynchronous not found — skipping forced scan.");
        }

        UFunction* fn = iface->GetFunction("AssetRegistry", "GetAssetsByPath");
        if (!fn)
        {
            if (!quiet) warn("[BpModLoader] (registry) GetAssetsByPath not found on the AssetRegistry interface class — skipping registry pass.");
            return;
        }

        Params::AssetRegistry_GetAssetsByPath parms{};
        parms.PackagePath              = FName(kModPath);
        parms.bRecursive               = true;
        parms.bIncludeOnlyOnDiskAssets = false;

        const auto savedFlags = fn->FunctionFlags;
        fn->FunctionFlags |= 0x400;                     // mirror the generated wrappers
        registryObj->ProcessEvent(fn, &parms);
        fn->FunctionFlags = savedFlags;

        TArray<FAssetData>& assets = parms.OutAssetData;
        if (!quiet)
            info("[BpModLoader] (registry) GetAssetsByPath('{}') → {} asset(s).",
                 StringLib::ToNarrow(kModPath), assets.Num());

        for (int i = 0; i < assets.Num(); ++i)
        {
            const FAssetData& ad   = assets[i];
            std::string       name = ad.AssetName.ToString();
            if (!quiet && kVerbose)
                info("[BpModLoader]   (registry) asset[{}] = '{}'", i, name);

            if (!StartsWith(name, kNamePrefix)) continue;

            UClass* cls = ResolveAssetClass(ad, name);
            if (!cls)                                   { if (!quiet) warn("[BpModLoader]   (registry) could not resolve class for '{}'.", name); continue; }
            if (!cls->IsSubclassOf(AActor::StaticClass())) { if (!quiet) warn("[BpModLoader]   (registry) '{}' is not an Actor — skipping.", name); continue; }
            out.insert(cls);
        }
    }

    // ── Discovery pass B: live object table (catches already-loaded classes) ─────
    static void CollectFromGObjects(std::unordered_set<UClass*>& out,
                                    std::vector<std::string>& hints, bool quiet)
    {
        UClass* actorClass = AActor::StaticClass();
        int scanned = 0, actorClasses = 0;

        const int num = UObject::GObjects->Num();
        for (int i = 0; i < num; ++i)
        {
            UObject* obj = UObject::GObjects->GetByIndex(i);
            if (!obj || !IsValidRaw(obj))         continue;   // flag check — no ProcessEvent
            if (!obj->IsA(UClass::StaticClass())) continue;
            ++scanned;

            UClass* cls = static_cast<UClass*>(obj);
            if (!cls->IsSubclassOf(actorClass))   continue;
            ++actorClasses;

            std::string name = cls->GetName();
            if (StartsWith(name, "SKEL_") || StartsWith(name, "REINST_") ||
                StartsWith(name, "TRASHCLASS_") || StartsWith(name, "PLACEHOLDER-"))
                continue;

            if (!StartsWith(name, kNamePrefix))
            {
                if (!quiet && kVerbose)
                    if (hints.size() < 40 && EndsWith(name, "_C") &&
                        (ContainsCI(name, "init") || ContainsCI(name, "mod")))
                        hints.push_back(name);
                continue;
            }
            out.insert(cls);
        }
        if (!quiet)
            info("[BpModLoader] (gobjects) scanned {} class(es), {} Actor class(es).",
                 scanned, actorClasses);
    }

    // True if a live (non-CDO) instance of exactly this class already exists in
    // the object table.  Keeps the loader idempotent across hot-reloads (spawned
    // actors persist when the DLL reloads) and avoids piling onto instances another
    // loader already created.  Raw scan — flag check only, no ProcessEvent.
    static bool HasLiveInstance(UClass* cls)
    {
        const int num = UObject::GObjects->Num();
        for (int i = 0; i < num; ++i)
        {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!o || !IsValidRaw(o) || o->IsDefaultObject()) continue;
            if (o->Class == cls) return true;
        }
        return false;
    }

    // ── One discovery+spawn pass.  Spawns only classes not already spawned this
    //    world (s_spawned).  quiet=true suppresses the verbose diagnostics used
    //    by the periodic rescans — only actual new spawns are logged. ───────────
    static void RunDiscovery(UWorld* world, bool quiet)
    {
        std::unordered_set<UClass*> candidates;
        std::vector<std::string>    hints;

        CollectFromRegistry(candidates, quiet);          // self-sufficient: discovers + loads from disk
        CollectFromGObjects(candidates, hints, quiet);   // anything already loaded

        const FTransform spawnXf = IdentityTransform();
        int spawned = 0;
        for (UClass* cls : candidates)
        {
            if (!s_spawned.insert(cls).second) continue; // already handled this world

            std::string name = cls->GetName();

            // Idempotency: don't add a second instance if one already exists
            // (persisted across a hot-reload, or spawned by another loader).
            if (HasLiveInstance(cls))
            {
                if (!quiet) info("[BpModLoader] '{}' already has a live instance — skipping spawn.", name);
                continue;
            }

            AActor* actor = UGameplayStatics::BeginDeferredActorSpawnFromClass(
                world,
                TSubclassOf<AActor>(cls),
                spawnXf,
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
                /*Owner=*/nullptr,
                ESpawnActorScaleMethod::SelectDefaultAtRuntime);

            if (!actor)
            {
                warn("[BpModLoader] BeginDeferredActorSpawnFromClass returned null for '{}' (abstract class?).", name);
                continue;
            }
            UGameplayStatics::FinishSpawningActor(actor, spawnXf,
                ESpawnActorScaleMethod::SelectDefaultAtRuntime);
            info("[BpModLoader] OK Spawned '{}'.", name);
            ++spawned;
        }

        if (!quiet)
        {
            info("[BpModLoader] Pass done — {} new spawn(s), {} total this world (world '{}').",
                 spawned, s_spawned.size(), world->GetName());

            if (s_spawned.empty() && !hints.empty())
            {
                info("[BpModLoader] Loaded Actor BP classes containing 'init'/'mod' "
                     "(rename to '{}*' or change kNamePrefix):", kNamePrefix);
                for (const auto& h : hints)
                    info("[BpModLoader]     {}", h);
            }
        }
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
        s_LastWorld = nullptr;  // force a pass on the very first tick

        s_TickHandle = GameHooks::OnEngineTick(
            [](UEngine*, float DeltaSeconds, bool)
            {
                // Guard against the one stale tick that can fire after
                // RemoveCallback() — it dispatches from the pre-removal
                // snapshot but Uninstall() has already zeroed s_TickHandle.
                if (s_TickHandle == 0) return;

                UWorld* world = UWorld::GetWorld();
                if (!world) return;

                // Wait until the local controller is present — ensures the
                // level has finished its initial setup before we spawn.
                if (!IsValidOf<APlayerController>(GetLocalController())) return;

                // New world: reset per-world state and run a full (loud) pass.
                if (world != s_LastWorld)
                {
                    s_LastWorld   = world;
                    s_spawned.clear();
                    s_rescanAccum = 0.f;
                    s_sinceWorld  = 0.f;

                    info("[BpModLoader] Level transition → world '{}'. Watching for InitMod_ "
                         "actors for {:.0f}s (late loaders included).", world->GetName(), kRescanWindow);
                    RunDiscovery(world, /*quiet=*/false);

                    for (auto& [h, fn] : s_WorldCallbacks)
                        fn(world);
                    return;
                }

                // Same world: keep catching late-loading mods for a window.
                if (s_sinceWorld > kRescanWindow) return;
                s_sinceWorld  += DeltaSeconds;
                s_rescanAccum += DeltaSeconds;
                if (s_rescanAccum >= kRescanInterval)
                {
                    s_rescanAccum = 0.f;
                    RunDiscovery(world, /*quiet=*/true);   // silent unless it spawns something
                }
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
        s_spawned.clear();
        s_WorldCallbacks.clear();
        info("[BpModLoader] Uninstalled.");
    }

} // namespace BpModLoader
