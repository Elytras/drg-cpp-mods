// Aim.cpp — Aimbot + RCS + Silent Aim implementation.
// See Aim.h for the public surface.

#include "Aim.h"
#include "Library.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "SDK/SDK/Basic.hpp"
#include "SDK/SDK/CoreUObject_classes.hpp"
#include "SDK/SDK/Engine_classes.hpp"
#include "SDK/SDK/FSD_classes.hpp"
#include "SDK/SDK/FSD_parameters.hpp"

extern HANDLE g_hRespEvent;

using namespace SDK;
using namespace ObjectCast;
using namespace GameHooks;

namespace AimAssist
{
    // ─────────────────────────────────────────────────────────────────────────
    //  Target selection — built-in selectors + name-keyed registry. Custom
    //  selectors register themselves via Targeting::Register(). Config layer
    //  references them by name (set "TargetSelector" in WeaponConfigOverride).
    //
    //  TargetCandidate carries the metadata each selector needs (FOV/LOS/WP
    //  flags + distance/dot tiebreakers). FindAimbotTarget builds the
    //  candidate list and the resolved selector picks one.
    //
    //  Future stateful selectors (e.g. "hit-all-once" keyed on reload, or
    //  "status-priority" keyed on applied status effects) will need a
    //  per-weapon state hook + reload/hit observation — deferred.
    // ─────────────────────────────────────────────────────────────────────────

    namespace Targeting
    {
        struct TargetCandidate
        {
            SDK::AFSDPawn* enemy = nullptr;
            float dot           = -2.f;       // dot(forward, normalize(enemy - cam)); higher = closer to crosshair
            float distSq        = FLT_MAX;    // |enemy - cam|^2
            bool  nearCrosshair = false;      // dot >= cos(FOV/2)
            bool  bodyVisible   = false;      // cam→actor LOS clear (only computed when nearCrosshair)
            bool  hasWP         = false;      // phys asset declares any non-destroyed weakpoint
            bool  wpVisible     = false;      // at least one WP candidate has LOS (only when nearCrosshair && bodyVisible && hasWP)
        };

        using SelectorFn = std::function<SDK::AFSDPawn*(const std::vector<TargetCandidate>&)>;

        // ── Built-in: 5-tier priority (the documented default rule) ──────────
        //   1: nearCrosshair && bodyVisible && wpVisible
        //   2: nearCrosshair && bodyVisible && hasWP && !wpVisible
        //   3: nearCrosshair && bodyVisible && !hasWP
        //   4: otherwise && hasWP        (sorted by closest)
        //   5: otherwise && !hasWP       (sorted by closest)
        // Tier 1-3 tiebreak by dot (closer to crosshair); tier 4-5 by distance.
        inline SDK::AFSDPawn* DefaultPriority(const std::vector<TargetCandidate>& cs)
        {
            auto tier = [](const TargetCandidate& c) -> int {
                if (c.nearCrosshair && c.bodyVisible)
                {
                    if (c.wpVisible) return 1;
                    if (c.hasWP)     return 2;
                    return 3;
                }
                return c.hasWP ? 4 : 5;
            };
            SDK::AFSDPawn* best = nullptr;
            int   bestTier  = INT_MAX;
            float bestDot   = -2.f;
            float bestDist  = FLT_MAX;
            for (const auto& c : cs)
            {
                const int t = tier(c);
                if (t > bestTier) continue;
                const bool better = (t < bestTier)
                    || (t <= 3 ? c.dot > bestDot : c.distSq < bestDist);
                if (better) { best = c.enemy; bestTier = t; bestDot = c.dot; bestDist = c.distSq; }
            }
            return best;
        }

        // ── Built-in: closest enemy regardless of FOV/LOS ────────────────────
        inline SDK::AFSDPawn* ClosestAny(const std::vector<TargetCandidate>& cs)
        {
            SDK::AFSDPawn* best = nullptr; float bestDist = FLT_MAX;
            for (const auto& c : cs)
                if (c.distSq < bestDist) { best = c.enemy; bestDist = c.distSq; }
            return best;
        }

        // ── Built-in: closest enemy with a weakpoint, falling back to any ────
        inline SDK::AFSDPawn* ClosestWithWP(const std::vector<TargetCandidate>& cs)
        {
            SDK::AFSDPawn* best = nullptr; float bestDist = FLT_MAX;
            for (const auto& c : cs)
            {
                if (!c.hasWP) continue;
                if (c.distSq < bestDist) { best = c.enemy; bestDist = c.distSq; }
            }
            return best ? best : ClosestAny(cs);
        }

        // ── Registry ─────────────────────────────────────────────────────────
        inline std::unordered_map<std::string, SelectorFn>& Registry()
        {
            static std::unordered_map<std::string, SelectorFn> map;
            return map;
        }

        // Add a custom selector at runtime, e.g. for an overclock-specific rule:
        //   Targeting::Register("status-priority", [statusTag](const auto& cs) { ... });
        inline void Register(const std::string& name, SelectorFn fn)
        {
            Registry()[name] = std::move(fn);
        }

        inline void RegisterBuiltinsOnce()
        {
            static std::once_flag once;
            std::call_once(once, []
            {
                auto& r = Registry();
                r["default"]    = DefaultPriority;
                r["closest"]    = ClosestAny;
                r["closest-wp"] = ClosestWithWP;
            });
        }

        inline SelectorFn Get(const std::string& name)
        {
            RegisterBuiltinsOnce();
            auto& r = Registry();
            if (auto it = r.find(name); it != r.end()) return it->second;
            warn("[targeting] unknown selector '{}', using 'default'", name);
            return DefaultPriority;
        }
    } // namespace Targeting

    // ─────────────────────────────────────────────────────────────────────────
    //  Tunables — all aimbot/RCS/silent-aim constants live here. Edit and rebuild.
    // ─────────────────────────────────────────────────────────────────────────

    namespace Config
    {
        // ── FOV cones (degrees) ──────────────────────────────────────────────
        // Aimbot snap (when MouseLeft held, silent aim off) — wider cone since
        // the camera is being moved to the target anyway.
        inline constexpr float AimbotFOVDeg     = 0.f;
        // Silent aim (hitscan + projectile) — tighter cone so wildly off-target
        // shots don't get redirected.
        inline constexpr float SilentAimFOVDeg  = 360.f;

        // ── Weakpoint sampling ───────────────────────────────────────────────
        // Multiplier applied to a phys-body sphere/sphyl radius when picking
        // the test radius for multipoint WP visibility sampling. <1 keeps
        // samples comfortably inside the WP volume.
        inline constexpr float BodyRadiusScale    = 0.75f;
        // Used when a phys body has no sphere/sphyl element (rare). ~15 UU ≈ 15 cm.
        inline constexpr float BodyRadiusFallback = 15.f;

        // ── Multipoint WP visibility offsets (sphere of radius R around center) ──
        // Tried in order; first visible candidate wins as the aim position.
        // `inline const` rather than constexpr — FVector isn't a literal type in the SDK.
        inline const std::array<FVector, 7> WpSampleOffsets{ {
            { 0,  0,  0},
            { 1,  0,  0}, {-1,  0,  0},
            { 0,  1,  0}, { 0, -1,  0},
            { 0,  0,  1}, { 0,  0, -1},
        } };

        // ── RCS gimbal-flip threshold (degrees) ──────────────────────────────
        // Single-frame yaw delta or cam↔ctrl offset above this is treated as
        // an engine pitch-wrap flip, not real recoil/input. State resyncs.
        inline constexpr float GimbalFlipThresholdDeg = 90.f;

        // ── Silent aim: ignore weakpoint line-of-sight ───────────────────────
        // Aimbot snap *always* requires LOS (moving the camera to an unseen
        // point looks bad), but silent aim doesn't move the camera and just
        // tells the server "I hit this WP" — so occlusion from the camera's
        // angle is irrelevant. With this on, silent aim commits to the chosen
        // WP even if the player can't see it (e.g. enemy turned away).
        inline constexpr bool SilentAimRequireLOS = false;

        // ── Equipped-item classes that disable aimbot/silent aim ─────────────
        // (Mining tools, traversal gear — anything that isn't a damage-dealing
        // weapon.) Lazy-built on first access since StaticClass() isn't constexpr.
        inline const std::vector<SDK::FName>& IgnoredItemClasses()
        {
            static const std::vector<SDK::FName> cache = [] {
                std::vector<SDK::FName> v;
                for (SDK::UClass* cls : {
                        SDK::APickaxeItem::StaticClass(),
                        SDK::ADoubleDrillItem::StaticClass(),
                        SDK::AZipLineItem::StaticClass(),
                        SDK::AGrapplingHookGun::StaticClass(),
                    })
                    if (cls) v.push_back(cls->Name);
                v.push_back(SDK::FName(L"WPN_PlatformGun_C"));
                return v;
            }();
            return cache;
        }

        // ── Default entries for the mutable IgnoreBaseClasses list ───────────
        // Seeded on first FindAimbotTarget call if the list is still empty.
        inline const std::vector<SDK::FName>& DefaultIgnoreBaseClasses()
        {
            static const std::vector<SDK::FName> cache = {
                SDK::FName(L"SDK::ENE_Spider_Grunt_Base_C"),
            };
            return cache;
        }

        // ─────────────────────────────────────────────────────────────────────
        //  Per-weapon overrides — Phase 1 of the hierarchical config.
        //
        //  Resolution: weapon-class override → global default. Lists replace
        //  the parent layer's list (no merging) — set IgnoreBaseClasses on an
        //  override to use that list exactly for that weapon.
        //
        //  Storage: a `std::string` keyed map of `WeaponConfigOverride`. C++
        //  defaults set in EnsureOverridesLoaded(); a JSON file next to the
        //  DLL (`aim_config.json`) — if present — augments / overrides them.
        // ─────────────────────────────────────────────────────────────────────

        struct WeaponConfigOverride
        {
            std::optional<float> AimbotFOVDeg;
            std::optional<float> SilentAimFOVDeg;
            std::optional<bool>  SilentAimRequireLOS;
            std::optional<std::vector<SDK::FName>> IgnoreBaseClasses;
            std::optional<std::vector<SDK::FName>> ForceIncludeClasses;
            // Name of a selector registered in Targeting::Registry().
            // "default" if missing/unknown. Built-ins: default, closest, closest-wp.
            std::optional<std::string> TargetSelector;
        };

        inline std::unordered_map<std::string, WeaponConfigOverride>& WeaponOverridesRef()
        {
            static std::unordered_map<std::string, WeaponConfigOverride> map;
            return map;
        }

        // Parse aim_config.json. Schema:
        //   { "weapons": { "WPN_X_C": { "AimbotFOVDeg": 45.0, ...,
        //                                "IgnoreBaseClasses": ["ENE_Y_C", ...] } } }
        // Unknown keys are ignored; bad types are skipped with a warning.
        inline void LoadOverridesFromJSON(const std::filesystem::path& path)
        {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) return;
            try
            {
                std::ifstream ifs(path);
                nlohmann::json j;
                ifs >> j;
                if (!j.contains("weapons") || !j["weapons"].is_object())
                {
                    warn("[aim config] {} missing top-level 'weapons' object", path.string());
                    return;
                }
                auto& map = WeaponOverridesRef();
                int loaded = 0;
                for (auto it = j["weapons"].begin(); it != j["weapons"].end(); ++it)
                {
                    const std::string& className = it.key();
                    const auto& w = it.value();
                    if (!w.is_object()) continue;

                    WeaponConfigOverride& wc = map[className];  // create-or-overwrite
                    if (auto k = w.find("AimbotFOVDeg");        k != w.end() && k->is_number())  wc.AimbotFOVDeg        = k->get<float>();
                    if (auto k = w.find("SilentAimFOVDeg");     k != w.end() && k->is_number())  wc.SilentAimFOVDeg     = k->get<float>();
                    if (auto k = w.find("SilentAimRequireLOS"); k != w.end() && k->is_boolean()) wc.SilentAimRequireLOS = k->get<bool>();

                    auto parseList = [](const nlohmann::json& arr) {
                        std::vector<SDK::FName> v;
                        for (const auto& e : arr)
                            if (e.is_string())
                                v.push_back(SDK::FName(StringLib::ToWide(e.get<std::string>()).c_str()));
                        return v;
                    };
                    if (auto k = w.find("IgnoreBaseClasses");   k != w.end() && k->is_array())  wc.IgnoreBaseClasses   = parseList(*k);
                    if (auto k = w.find("ForceIncludeClasses"); k != w.end() && k->is_array())  wc.ForceIncludeClasses = parseList(*k);
                    if (auto k = w.find("TargetSelector");      k != w.end() && k->is_string()) wc.TargetSelector      = k->get<std::string>();
                    ++loaded;
                }
                info("[aim config] loaded {} weapon overrides from {}", loaded, path.string());
            }
            catch (const std::exception& e)
            {
                warn("[aim config] JSON load failed: {}", e.what());
            }
        }

        // First-call loader: seed any C++ hardcoded defaults, then merge the
        // optional JSON file on top.
        inline void EnsureOverridesLoaded()
        {
            static std::once_flag once;
            std::call_once(once, []
            {
                // ── C++ hardcoded defaults — uncomment / extend as needed ────
                // auto& m = WeaponOverridesRef();
                // m["WPN_SMG_C"]    = { .SilentAimFOVDeg = 45.f };
                // m["WPN_Sniper_C"] = { .SilentAimFOVDeg = 10.f, .SilentAimRequireLOS = true };

                // ── JSON override (if file present alongside the DLL) ────────
                wchar_t modulePath[MAX_PATH]{};
                HMODULE hMod = GetModuleHandleW(L"DrgMods.dll");
                if (hMod && GetModuleFileNameW(hMod, modulePath, MAX_PATH))
                {
                    std::filesystem::path p = modulePath;
                    LoadOverridesFromJSON(p.parent_path() / "aim_config.json");
                }
            });
        }

        // ── Resolve helpers (weapon override → global default) ───────────────
        inline float ResolveAimbotFOV(SDK::AItem* eq)
        {
            EnsureOverridesLoaded();
            if (eq && eq->Class)
            {
                auto& m = WeaponOverridesRef();
                if (auto it = m.find(eq->Class->Name.ToString()); it != m.end() && it->second.AimbotFOVDeg)
                    return *it->second.AimbotFOVDeg;
            }
            return AimbotFOVDeg;
        }
        inline float ResolveSilentAimFOV(SDK::AItem* eq)
        {
            EnsureOverridesLoaded();
            if (eq && eq->Class)
            {
                auto& m = WeaponOverridesRef();
                if (auto it = m.find(eq->Class->Name.ToString()); it != m.end() && it->second.SilentAimFOVDeg)
                    return *it->second.SilentAimFOVDeg;
            }
            return SilentAimFOVDeg;
        }
        inline bool ResolveSilentAimRequireLOS(SDK::AItem* eq)
        {
            EnsureOverridesLoaded();
            if (eq && eq->Class)
            {
                auto& m = WeaponOverridesRef();
                if (auto it = m.find(eq->Class->Name.ToString()); it != m.end() && it->second.SilentAimRequireLOS)
                    return *it->second.SilentAimRequireLOS;
            }
            return SilentAimRequireLOS;
        }
        inline Targeting::SelectorFn ResolveTargetSelector(SDK::AItem* eq)
        {
            EnsureOverridesLoaded();
            if (eq && eq->Class)
            {
                auto& m = WeaponOverridesRef();
                if (auto it = m.find(eq->Class->Name.ToString()); it != m.end() && it->second.TargetSelector)
                    return Targeting::Get(*it->second.TargetSelector);
            }
            return Targeting::Get("default");
        }
    } // namespace Config

    // ─────────────────────────────────────────────────────────────────────────
    //  Debug toggles — compile-time flags that gate verbose diagnostic logging.
    //  Flip and rebuild to investigate weapons whose silent aim isn't working;
    //  the compiler strips the log calls entirely when these are false.
    // ─────────────────────────────────────────────────────────────────────────

    namespace Debug
    {
        // Verbose silent aim logging — every hook entry, validation gate,
        // target lookup result, and param modification gets an info() line.
        // Use to diagnose "this weapon doesn't get silent-aimed" by watching
        // which hook fires and what data it sees on each shot.
        inline constexpr bool LogSilentAim = false;

        // GetComponentByClass behavior in `firespy`:
        //   false → drop the line (default; these calls are high-volume noise).
        //   true  → log it with the queried class name, e.g.
        //           [firespy] WPN_X::GetComponentByClass(class=UDamageComponent).
        inline constexpr bool ShowGetComponentByClassQuery = false;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Public state
    // ─────────────────────────────────────────────────────────────────────────

    std::vector<SDK::FName> IgnoreBaseClasses{};
    std::vector<SDK::FName> ForceIncludeClasses{};

    bool   RecoilEnabled = false;
    float  RCSFactor = 1.0f;
    bool   AimbotKeyHeld = false;
    bool   SilentAimEnabled = false;

    // ─────────────────────────────────────────────────────────────────────────
    //  Internal state — only the implementation touches these.
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        bool   RCSInitialized = false;
        float  RCSDesiredPitch = 0.f;
        float  RCSPrevCtrlPitch = 0.f;
        float  RCSDesiredYaw = 0.f;
        float  RCSPrevCtrlYaw = 0.f;

        CallbackHandle RCSHandle                = 0;   // OnEngineTick handle for the RCS/aimbot loop
        CallbackHandle HitscanSilentAimHandle   = 0;
        CallbackHandle HitscanRpcRedirectHandle = 0;
        CallbackHandle HitscanHitOptimizeHandle = 0;

        // Set by the OnWeaponFired hook when a shot was redirected, consumed by
        // the Server_RegisterHit_Terrain hook to repackage the terrain RPC into
        // an enemy hit on the same component. Game-thread only — no sync needed.
        struct HitscanRedirect
        {
            bool                        active  = false;
            SDK::UPrimitiveComponent*   target  = nullptr;  // mesh component owning the WP
            SDK::UFSDPhysicalMaterial*  physMat = nullptr;  // chosen WP phys mat (null for body fallback)
        };
        HitscanRedirect g_PendingRedirect{};
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Damage / mesh helpers — used to gate aimbot to weapons that can deal
    //  damage and to walk enemy skeletal meshes.
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        struct FDamageInfo
        {
            float Damage = 0.f;
            float RadialDamage = 0.f;
            bool IsValid() const { return Damage > 0.001f || RadialDamage > 0.01f; }
        };

        bool ExtractDamageInfo(SDK::UDamageComponent* DamageComponent, FDamageInfo& OutInfo)
        {
            if (!DamageComponent) return false;
            OutInfo.Damage = DamageComponent->Damage;
            OutInfo.RadialDamage = DamageComponent->RadialDamage;
            return true;
        }

        bool GetDamageInfoFromProjectileClass(SDK::UClass* ProjectileClass, FDamageInfo& OutInfo)
        {
            if (!ProjectileClass) return false;
            SDK::AProjectileBase* ProjectileCDO = Cast<SDK::AProjectileBase>(ProjectileClass->ClassDefaultObject);
            if (!ProjectileCDO) return false;

            if (ExtractDamageInfo(
                Cast<SDK::UDamageComponent>(ProjectileCDO->GetComponentByClass(SDK::UDamageComponent::StaticClass())),
                OutInfo))
                return true;

            // Components created at runtime aren't on the CDO — spawn a throwaway
            // projectile far off-grid and read its damage component.
            SDK::FTransform Transform{};
            Transform.Translation = SDK::FVector{ 9999999, 9999999, 9999999 };

            SDK::AProjectileBase* Spawned = nullptr;
            if (!SpawnActor<SDK::AProjectileBase>(ProjectileClass, Transform, Spawned))
                return false;

            const bool ok = ExtractDamageInfo(
                Cast<SDK::UDamageComponent>(Spawned->GetComponentByClass(SDK::UDamageComponent::StaticClass())),
                OutInfo);
            Spawned->K2_DestroyActor();
            return ok;
        }

        // GetMesh() on SDK::AEnemyPawn is a BlueprintImplementableEvent with no C++ body — always null.
        // Use K2_GetComponentsByClass for SDK::AEnemyPawn; direct field for SDK::ADeepPathfinderCharacter.
        std::vector<SDK::USkeletalMeshComponent*> GetEnemyMeshes(SDK::AFSDPawn* Enemy)
        {
            if (auto* dp = Cast<SDK::AEnemyDeepPathfinderCharacter>(Enemy))
                return dp->Mesh ? std::vector<SDK::USkeletalMeshComponent*>{ dp->Mesh }
            : std::vector<SDK::USkeletalMeshComponent*>{};

            std::vector<SDK::USkeletalMeshComponent*> result;
            auto comps = Enemy->K2_GetComponentsByClass(SDK::USkeletalMeshComponent::StaticClass());
            for (int32 i = 0; i < comps.Num(); ++i)
                if (auto* smc = Cast<SDK::USkeletalMeshComponent>(comps[i]))
                    result.push_back(smc);
            return result;
        }

        // Largest sphere/capsule radius from the body's aggregate geometry,
        // scaled by Config::BodyRadiusScale; falls back to Config::BodyRadiusFallback.
        float GetBodyRadius(SDK::USkeletalBodySetup* Body)
        {
            float r = 0.f;
            for (int32 i = 0; i < Body->AggGeom.SphereElems.Num(); ++i)
                r = std::max(r, Body->AggGeom.SphereElems[i].Radius);
            for (int32 i = 0; i < Body->AggGeom.SphylElems.Num(); ++i)
                r = std::max(r, Body->AggGeom.SphylElems[i].Radius);
            return r > 0.f ? r * Config::BodyRadiusScale : Config::BodyRadiusFallback;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Visibility + breakable-weakpoint state
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        // Returns true if the weakpoint physics body identified by BoneName is visible
        // from CamLoc. Uses physics-body trace (bTraceComplex=false) so SDK::FHitResult::BoneName
        // is populated — direct name match beats a fragile distance threshold.
        bool IsWeakpointVisible(SDK::APlayerCharacter* LocalPlayer, SDK::AFSDPawn* Enemy,
            const FVector& CamLoc, const FVector& WPos, SDK::FName BoneName)
        {
            SDK::TArray<SDK::AActor*> NoIgnore;
            SDK::FHitResult Hit;
            const bool bHit = SDK::UKismetSystemLibrary::LineTraceSingle(
                LocalPlayer, CamLoc, WPos,
                SDK::ETraceTypeQuery::TraceTypeQuery1, false,
                NoIgnore, SDK::EDrawDebugTrace::None,
                &Hit, true,
                FLinearColor{}, FLinearColor{}, 0.f);

            if (!bHit) return true;
            if (Hit.Actor.Get() != static_cast<SDK::AActor*>(Enemy)) return false;
            return Hit.BoneName == BoneName;
        }

        // Cheap body-visibility check: line trace from camera to the enemy's
        // actor location. "Visible" means the first thing the trace hits is the
        // enemy itself (or nothing — a clear line). Used for tier 1-3 gating.
        bool IsBodyVisible(SDK::APlayerCharacter* LocalPlayer, SDK::AFSDPawn* Enemy,
            const FVector& CamLoc)
        {
            SDK::TArray<SDK::AActor*> NoIgnore;
            SDK::FHitResult Hit;
            const FVector Target = Enemy->K2_GetActorLocation();
            const bool bHit = SDK::UKismetSystemLibrary::LineTraceSingle(
                LocalPlayer, CamLoc, Target,
                SDK::ETraceTypeQuery::TraceTypeQuery1, false,
                NoIgnore, SDK::EDrawDebugTrace::None,
                &Hit, true,
                FLinearColor{}, FLinearColor{}, 0.f);
            if (!bHit) return true;
            return Hit.Actor.Get() == static_cast<SDK::AActor*>(Enemy);
        }

        struct BreakableWpState
        {
            bool  isBreakable = false;
            float health = -1.f;
            bool  isDestroyed = false;
        };

        // UE4 4.27 SDK::TMap AllocationFlags: inline uint32[4] starts at map_base+0x10.
        bool TMapSlotValid(const void* m, int32 i)
        {
            if (i < 0 || i >= 128) return false;
            const auto* f = reinterpret_cast<const uint32*>(static_cast<const uint8*>(m) + 0x10);
            return (f[i >> 5] >> (i & 31)) & 1u;
        }

        // SDK::TMap<SDK::FName(8), Value(0x18)> stride: sizeof(TSetElement<TPair<K,V>>) = 40 bytes.
        //   slot+0x00: SDK::FName key   slot+0x08: Value (24 bytes)
        //   slot+0x20: HashNextId  slot+0x24: HashIndex
        BreakableWpState GetBreakableWpState(SDK::AFSDPawn* Enemy, SDK::USkeletalMeshComponent* Mesh,
            SDK::FName BoneName, SDK::UFSDPhysicalMaterial* PhysMat)
        {
            BreakableWpState r;

            auto comps = Enemy->K2_GetComponentsByClass(SDK::UBaseArmorDamageComponent::StaticClass());
            for (int32 ci = 0; ci < comps.Num(); ++ci)
            {
                auto* base = Cast<SDK::UBaseArmorDamageComponent>(comps[ci]);
                if (!base || base->Mesh != Mesh) continue;

                bool manages = false;
                for (int32 mi = 0; mi < base->ArmorPhysMats.Num(); ++mi)
                    if (base->ArmorPhysMats[mi] == PhysMat) { manages = true; break; }
                if (!manages) continue;

                r.isBreakable = true;

                if (auto* hc = Cast<SDK::UArmorHealthDamageComponent>(base))
                {
                    const void* mp = &hc->PhysBoneToArmor;
                    const int32  nSlots = *reinterpret_cast<const int32*>(static_cast<const uint8*>(mp) + 0x08);
                    const uint8* buf = *reinterpret_cast<uint8* const*>(mp);
                    if (!buf) break;

                    for (int32 i = 0; i < nSlots; ++i)
                    {
                        if (!TMapSlotValid(mp, i)) continue;
                        const uint8* slot = buf + i * 40;
                        const SDK::FArmorHealthItem* item = reinterpret_cast<const SDK::FArmorHealthItem*>(slot + 8);
                        const bool              byMask = (hc->ArmorDamageInfo.ArmorIndexMask >> item->MaterialIndex) & 1;

                        for (int32 bi = 0; bi < item->ArmorBones.Num(); ++bi)
                        {
                            const SDK::FArmorHealthSubItem& sub = item->ArmorBones[bi];
                            bool match = (sub.BoneName == BoneName);
                            if (!match)
                                for (int32 ai = 0; !match && ai < sub.AdditionalBones.Num(); ++ai)
                                    if (sub.AdditionalBones[ai] == BoneName) match = true;
                            if (!match) continue;
                            r.health = sub.Health;
                            r.isDestroyed = byMask || sub.Health <= 0.f;
                            return r;
                        }
                    }
                }
                else if (auto* sc = Cast<SDK::USimpleArmorDamageComponent>(base))
                {
                    const void* mp = &sc->PhysBoneToArmor;
                    const int32  nSlots = *reinterpret_cast<const int32*>(static_cast<const uint8*>(mp) + 0x08);
                    const uint8* buf = *reinterpret_cast<uint8* const*>(mp);
                    if (!buf) break;

                    for (int32 i = 0; i < nSlots; ++i)
                    {
                        if (!TMapSlotValid(mp, i)) continue;
                        const uint8* slot = buf + i * 40;
                        const SDK::FDestructableBodypartItem* item = reinterpret_cast<const SDK::FDestructableBodypartItem*>(slot + 8);

                        for (int32 bi = 0; bi < item->ArmorBones.Num(); ++bi)
                        {
                            if (item->ArmorBones[bi] != BoneName) continue;
                            r.isDestroyed = (sc->ArmorDamageInfo.ArmorIndexMask >> item->MaterialIndex) & 1;
                            return r;
                        }
                    }
                }
                break;
            }
            return r;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Weakpoint target selection (best WP across a pawn's meshes)
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        struct WpTargetResult
        {
            FVector pos = {};
            bool hasWeakpoints = false; // physics asset declares any WP body
            bool anyVisible = false; // ≥1 visible & not destroyed
            SDK::USkeletalMeshComponent* mesh = nullptr;     // mesh that owns the chosen WP (or first mesh as body fallback)
            SDK::UFSDPhysicalMaterial*   physMat = nullptr;  // chosen WP's phys material (null when falling back to body)
        };

        WpTargetResult GetWeakpointTarget(SDK::AFSDPawn* Enemy,
            const std::vector<SDK::USkeletalMeshComponent*>& Meshes,
            const FVector& CamLoc, const FVector& Forward,
            SDK::APlayerCharacter* LocalPlayer,
            bool RequireLOS)
        {
            WpTargetResult result;
            FVector BestPos = {};
            float   BestMult = -1.f;
            float   BestDot = -2.f;
            SDK::USkeletalMeshComponent* BestMesh    = nullptr;
            SDK::UFSDPhysicalMaterial*   BestPhysMat = nullptr;

            for (SDK::USkeletalMeshComponent* Mesh : Meshes)
            {
                if (!Mesh) continue;
                auto* SkelAsset = Mesh->SkeletalMesh;
                if (!SkelAsset) continue;
                auto* PhysAsset = SkelAsset->PhysicsAsset;
                if (!PhysAsset) continue;

                for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); ++i)
                {
                    SDK::USkeletalBodySetup* Body = PhysAsset->SkeletalBodySetups[i];
                    if (!Body || !Body->PhysMaterial) continue;

                    SDK::UFSDPhysicalMaterial* FSDMat = Cast<SDK::UFSDPhysicalMaterial>(Body->PhysMaterial);
                    if (!FSDMat || !FSDMat->IsWeakPoint) continue;

                    result.hasWeakpoints = true;

                    auto WpState = GetBreakableWpState(Enemy, Mesh, Body->BoneName, FSDMat);
                    if (WpState.isBreakable && WpState.isDestroyed) continue;

                    const FVector Center = Mesh->GetSocketLocation(Body->BoneName);
                    const float   R = GetBodyRadius(Body);

                    // VisPos defaults to bone pivot. With RequireLOS off, we
                    // commit to the bone pivot and skip the LOS trace entirely
                    // — silent aim doesn't care if the player can see the WP.
                    FVector VisPos   = Center;
                    bool    bVisible = true;
                    if (RequireLOS)
                    {
                        // Multipoint: try center then ±R on each world axis.
                        // First visible candidate wins as the aim position.
                        bVisible = false;
                        for (const auto& Off : Config::WpSampleOffsets)
                        {
                            FVector Candidate = { Center.X + Off.X * R, Center.Y + Off.Y * R, Center.Z + Off.Z * R };
                            if (IsWeakpointVisible(LocalPlayer, Enemy, CamLoc, Candidate, Body->BoneName))
                            {
                                VisPos = Candidate; bVisible = true; break;
                            }
                        }
                        if (!bVisible) continue;
                    }

                    FVector Dir = VisPos - CamLoc;
                    const float Dist = std::sqrt(Dir.X * Dir.X + Dir.Y * Dir.Y + Dir.Z * Dir.Z);
                    if (Dist < 1.f) continue;
                    Dir.X /= Dist; Dir.Y /= Dist; Dir.Z /= Dist;

                    const float Dot = Forward.X * Dir.X + Forward.Y * Dir.Y + Forward.Z * Dir.Z;
                    const float Mult = FSDMat->DamageMultiplier;

                    if (Mult > BestMult || (Mult >= BestMult && Dot > BestDot))
                    {
                        BestMult = Mult; BestDot = Dot; BestPos = VisPos;
                        BestMesh = Mesh; BestPhysMat = FSDMat;
                        result.anyVisible = true;
                    }
                }
            }

            if (result.anyVisible)
            {
                result.pos     = BestPos;
                result.mesh    = BestMesh;
                result.physMat = BestPhysMat;
            }
            else
            {
                // Body-center fallback — no visible weakpoint (occluded or destroyed).
                // Pick the first mesh so the Server_RegisterHit RPC has a Target component.
                result.pos     = Enemy->K2_GetActorLocation();
                result.mesh    = Meshes.empty() ? nullptr : Meshes.front();
                result.physMat = nullptr;
            }
            return result;
        }

        // ─── Best damage body on an enemy ────────────────────────────────────
        // Scans every phys body on every mesh, returns the one with the
        // highest DamageMultiplier strictly greater than MinMultiplier. Used
        // by the Server_RegisterHit redirect to upgrade an already-hit
        // location to a better body part on the same enemy (weakpoint OR any
        // mat with a multiplier — some bonus-damage zones aren't flagged
        // IsWeakPoint but still scale incoming damage).
        struct BestDamageBody
        {
            FVector pos = {};
            SDK::USkeletalMeshComponent* mesh    = nullptr;
            SDK::UFSDPhysicalMaterial*   physMat = nullptr;
            float                         multiplier = 0.f;
        };

        std::optional<BestDamageBody> FindBestDamageBody(SDK::AFSDPawn* Enemy, float MinMultiplier)
        {
            if (!Enemy) return std::nullopt;
            auto meshes = GetEnemyMeshes(Enemy);
            if (meshes.empty()) return std::nullopt;

            BestDamageBody best;
            best.multiplier = MinMultiplier;
            bool found = false;

            for (SDK::USkeletalMeshComponent* Mesh : meshes)
            {
                if (!Mesh || !Mesh->SkeletalMesh || !Mesh->SkeletalMesh->PhysicsAsset) continue;
                auto* PhysAsset = Mesh->SkeletalMesh->PhysicsAsset;
                for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); ++i)
                {
                    auto* Body = PhysAsset->SkeletalBodySetups[i];
                    if (!Body || !Body->PhysMaterial) continue;
                    auto* M = Cast<SDK::UFSDPhysicalMaterial>(Body->PhysMaterial);
                    if (!M) continue;

                    // Skip destroyed breakable weakpoints — broken armor zones
                    // no longer apply the multiplier (the underlying body
                    // material takes over server-side).
                    if (M->IsWeakPoint)
                    {
                        auto st = GetBreakableWpState(Enemy, Mesh, Body->BoneName, M);
                        if (st.isBreakable && st.isDestroyed) continue;
                    }

                    if (M->DamageMultiplier > best.multiplier)
                    {
                        best.multiplier = M->DamageMultiplier;
                        best.physMat    = M;
                        best.mesh       = Mesh;
                        best.pos        = Mesh->GetSocketLocation(Body->BoneName);
                        found = true;
                    }
                }
            }
            return found ? std::optional<BestDamageBody>{ best } : std::nullopt;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Best aim target across all enemies in the FOV cone
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        struct AimTargetInfo
        {
            FVector                       pos     = {};
            SDK::AFSDPawn*                enemy   = nullptr;
            SDK::USkeletalMeshComponent*  mesh    = nullptr;   // mesh that owns the chosen WP (for Server_RegisterHit Target)
            SDK::UFSDPhysicalMaterial*    physMat = nullptr;   // chosen WP's phys mat (null for body-center fallback)
        };

        std::optional<AimTargetInfo> FindAimbotTarget(SDK::APlayerCharacter* LocalPlayer,
            SDK::APlayerCameraManager* /*CamMgr*/,
            const FVector& CamLoc,
            const FVector& Forward,
            float FOVDeg,
            bool RequireLOS = true)
        {
            static constexpr float kDeg2Rad = 3.14159265f / 180.f;

            if (IsOnSpaceRig()) return std::nullopt;

            SDK::UInventoryComponent* Inventory = LocalPlayer->InventoryComponent;
            if (!Inventory) return std::nullopt;

            SDK::AItem* Equipped = Inventory->GetEquippedItem();
            if (!Equipped) return std::nullopt;

            // Equipped items that aren't damage-dealing weapons — aimbot stays
            // dormant for these (mining tools, traversal gear, etc).
            for (const SDK::FName& ClassName : Config::IgnoredItemClasses())
                if (IsChildOfByName(Equipped, ClassName)) return std::nullopt;

            FDamageInfo DamageInfo;
            if (SDK::UDamageComponent* DamageComponent = GetComponent<SDK::UDamageComponent>(Equipped))
                ExtractDamageInfo(DamageComponent, DamageInfo);
            else
            {
                SDK::UProjectileLauncherComponent* Launcher = GetComponent<SDK::UProjectileLauncherComponent>(Equipped);
                if (!Launcher) return std::nullopt;
                if (!GetDamageInfoFromProjectileClass(Launcher->ProjectileClass, DamageInfo)) return std::nullopt;
            }
            if (!DamageInfo.IsValid()) return std::nullopt;

            std::vector<SDK::AFSDPawn*> Enemies = GetAliveNonFriendlies();
            if (Enemies.empty()) return std::nullopt;

            const float HalfFOVCos = std::cos(FOVDeg * 0.5f * kDeg2Rad);

            // Lazy default-seed for IgnoreBaseClasses — users can append or
            // clear at any time after first call.
            if (IgnoreBaseClasses.empty()) [[unlikely]]
                IgnoreBaseClasses = Config::DefaultIgnoreBaseClasses();

            // Resolve effective ignore/force lists: a per-weapon override REPLACES
            // the global mutable list entirely (no merging). If no override is set,
            // we use the global lists (which the user can mutate at runtime).
            const std::vector<SDK::FName>* effIgnore = &IgnoreBaseClasses;
            const std::vector<SDK::FName>* effForce  = &ForceIncludeClasses;
            {
                Config::EnsureOverridesLoaded();
                auto& m = Config::WeaponOverridesRef();
                auto it = Equipped->Class ? m.find(Equipped->Class->Name.ToString()) : m.end();
                if (it != m.end())
                {
                    if (it->second.IgnoreBaseClasses)   effIgnore = &*it->second.IgnoreBaseClasses;
                    if (it->second.ForceIncludeClasses) effForce  = &*it->second.ForceIncludeClasses;
                }
            }

            // Build the candidate metadata that selectors consume. Expensive
            // bits (body LOS, WP LOS sampling) are gated by cheaper checks so
            // out-of-FOV enemies don't pay the cost of LOS traces.
            std::vector<Targeting::TargetCandidate> candidates;
            candidates.reserve(Enemies.size());

            for (SDK::AFSDPawn* Enemy : Enemies)
            {
                if (!IsValidOf<SDK::AFSDPawn>(Enemy)) continue;

                // Inclusion rules:
                //   1. ForceIncludeClasses (exact)         → always targeted.
                //   2. Elite / boss                        → bypass ignore list.
                //   3. IgnoreBaseClasses (subclass match)  → skipped.
                bool forceInclude = false;
                if (Enemy->Class)
                    for (const SDK::FName& name : *effForce)
                        if (Enemy->Class->Name == name) { forceInclude = true; break; }

                if (!forceInclude)
                {
                    bool eliteOrBoss = Enemy->IsElite();
                    if (!eliteOrBoss)
                        if (auto* hc = Cast<SDK::UEnemyHealthComponent>(Enemy->GetHealthComponent()))
                            eliteOrBoss = hc->bIsBossFight;

                    if (!eliteOrBoss)
                    {
                        bool ignored = false;
                        for (const SDK::FName& name : *effIgnore)
                            if (IsChildOfByName(Enemy, name)) { ignored = true; break; }
                        if (ignored) continue;
                    }
                }

                Targeting::TargetCandidate cand;
                cand.enemy = Enemy;

                FVector ToEnemy = Enemy->K2_GetActorLocation() - CamLoc;
                cand.distSq = ToEnemy.X * ToEnemy.X + ToEnemy.Y * ToEnemy.Y + ToEnemy.Z * ToEnemy.Z;
                const float Dist = std::sqrt(cand.distSq);
                if (Dist < 0.01f) continue;
                ToEnemy.X /= Dist; ToEnemy.Y /= Dist; ToEnemy.Z /= Dist;
                cand.dot = Forward.X * ToEnemy.X + Forward.Y * ToEnemy.Y + Forward.Z * ToEnemy.Z;
                cand.nearCrosshair = (cand.dot >= HalfFOVCos);

                // Cheap WP detection — any non-destroyed weakpoint phys body.
                auto meshes = GetEnemyMeshes(Enemy);
                for (SDK::USkeletalMeshComponent* Mesh : meshes)
                {
                    if (!Mesh || !Mesh->SkeletalMesh || !Mesh->SkeletalMesh->PhysicsAsset) continue;
                    auto* PhysAsset = Mesh->SkeletalMesh->PhysicsAsset;
                    for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num() && !cand.hasWP; ++i)
                    {
                        auto* Body = PhysAsset->SkeletalBodySetups[i];
                        if (!Body || !Body->PhysMaterial) continue;
                        auto* M = Cast<SDK::UFSDPhysicalMaterial>(Body->PhysMaterial);
                        if (!M || !M->IsWeakPoint) continue;
                        auto st = GetBreakableWpState(Enemy, Mesh, Body->BoneName, M);
                        if (st.isBreakable && st.isDestroyed) continue;
                        cand.hasWP = true;
                    }
                    if (cand.hasWP) break;
                }

                // Expensive checks gated on near-crosshair → only matter for tier 1-3.
                if (cand.nearCrosshair)
                {
                    cand.bodyVisible = IsBodyVisible(LocalPlayer, Enemy, CamLoc);
                    if (cand.bodyVisible && cand.hasWP)
                    {
                        // wpVisible distinguishes tier 1 from tier 2. We reuse
                        // GetWeakpointTarget with RequireLOS=true and read the
                        // anyVisible flag — same multipoint sampling logic.
                        auto probe = GetWeakpointTarget(Enemy, meshes, CamLoc, Forward, LocalPlayer, /*RequireLOS*/true);
                        cand.wpVisible = probe.anyVisible;
                    }
                }

                candidates.push_back(cand);
            }
            if (candidates.empty()) return std::nullopt;

            // Run the resolved selector. Default is the 5-tier priority.
            Targeting::SelectorFn selector = Config::ResolveTargetSelector(Equipped);
            SDK::AFSDPawn* chosen = selector(candidates);
            if (!chosen) return std::nullopt;

            // Pick the actual aim position on the selected enemy. RequireLOS
            // here controls WP picking (silent aim defaults to false → commit
            // to bone pivot even when occluded; aimbot snap → true).
            AimTargetInfo info;
            info.enemy = chosen;
            auto chosenMeshes = GetEnemyMeshes(chosen);
            if (chosenMeshes.empty())
            {
                info.pos = chosen->K2_GetActorLocation();
                return info;
            }
            auto wp = GetWeakpointTarget(chosen, chosenMeshes, CamLoc, Forward, LocalPlayer, RequireLOS);
            info.pos     = wp.pos;
            info.mesh    = wp.mesh;
            info.physMat = wp.physMat;
            return info;
        }

        std::optional<FVector> FindAimbotTargetPos(SDK::APlayerCharacter* LocalPlayer,
            SDK::APlayerCameraManager* CamMgr,
            const FVector& CamLoc,
            const FVector& Forward,
            float FOVDeg,
            bool RequireLOS = true)
        {
            auto t = FindAimbotTarget(LocalPlayer, CamMgr, CamLoc, Forward, FOVDeg, RequireLOS);
            if (!t) return std::nullopt;
            return t->pos;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Keybind flags
    // ─────────────────────────────────────────────────────────────────────────

    void AimbotPressed() { AimbotKeyHeld = true; }
    void AimbotReleased() { AimbotKeyHeld = false; }

    // ─────────────────────────────────────────────────────────────────────────
    //  Unified RCS + aimbot loop
    // ─────────────────────────────────────────────────────────────────────────

    void ToggleRecoilControl()
    {
        RecoilEnabled  = !RecoilEnabled;
        RCSInitialized = false;

        if (RecoilEnabled)
        {
            // UE4 pitch/yaw live in [0,360); normalize to [-180,180] so
            // arithmetic and clamps are geometrically correct.
            auto normP = [](float a) -> float {
                a = std::fmod(a, 360.f);
                if (a > 180.f)  a -= 360.f;
                if (a < -180.f) a += 360.f;
                return a;
                };

            // Driven from UEngine::Tick (post-original): the engine has just
            // finished its frame tick, so the camera/controller state we read
            // is from this frame and the rotation we write is what the next
            // frame's input pass picks up. Fires exactly once per frame, so
            // none of the per-PE camera-delta gating is needed anymore.
            RCSHandle = OnEngineTick(
                [normP](SDK::UEngine* /*Engine*/, float /*DeltaSeconds*/, bool /*bIdleMode*/)
                {
                    if (!RecoilEnabled) return;   // defensive — disable path already removes us

                    SDK::APlayerCharacter* Player = GetLocalPlayer();
                    if (!IsValidOf<SDK::APlayerCharacter>(Player)) return;

                    if (!Player->IsAlive()) return; 
                    static SDK::ECharacterState State = SDK::ECharacterState::Invalid;
                    
                    static auto IsValidState = [](SDK::ECharacterState s) {
                        return s == SDK::ECharacterState::Walking ||
                               s == SDK::ECharacterState::ZipLine ||
                               s == SDK::ECharacterState::Falling;
                        };
                    
                    if (!IsValidState(State))
                    {
                        //Delay RCS for one tick after respawn to avoid spinning
                        State = Player->GetCurrentState();
                        return;
                    }

                    State = Player->GetCurrentState();
                    if (!IsValidState(State)) return;

                    SDK::AFSDPlayerController* Ctrl = Cast<SDK::AFSDPlayerController>(Player->Controller);
                    if (!IsValidOf<SDK::AFSDPlayerController>(Ctrl)) return;

                    SDK::APlayerCameraManager* CamMgr = Ctrl->PlayerCameraManager;
                    if (!CamMgr) return;


                    const FRotator ctrlRot   = Ctrl->GetControlRotation();
                    const FRotator camRot    = CamMgr->CameraCachePrivate.POV.Rotation;
                    const float    ctrlPitch = normP(ctrlRot.Pitch);
                    const float    camPitch  = normP(camRot.Pitch);
                    const float    ctrlYaw   = normP(ctrlRot.Yaw);
                    const float    camYaw    = normP(camRot.Yaw);

                    if (!RCSInitialized)
                    {
                        RCSDesiredPitch  = ctrlPitch;
                        RCSPrevCtrlPitch = ctrlPitch;
                        RCSDesiredYaw    = ctrlYaw;
                        RCSPrevCtrlYaw   = ctrlYaw;
                        RCSInitialized   = true;
                        return;
                    }

                    // ── Gimbal-flip protection ─────────────────────────────
                    // When cam pitch crosses ±90°, UE reflects pitch and flips
                    // yaw by 180° to keep the look direction continuous. Both
                    // the cam↔ctrl yaw offset and the per-frame ctrl yaw delta
                    // spike to ~180° on that boundary; applying them as recoil
                    // compensation propagates as a 180° spin. Real recoil and
                    // real mouse input never produce a single-frame 90°+ yaw,
                    // so use that as the flip signal — resync and skip the frame.
                    const float ctrlPitchDelta = normP(ctrlPitch - RCSPrevCtrlPitch);
                    const float ctrlYawDelta   = normP(ctrlYaw   - RCSPrevCtrlYaw);
                    const float pitchOffset    = camPitch - ctrlPitch;
                    const float yawOffset      = normP(camYaw - ctrlYaw);
                    if (std::abs(yawOffset)    > Config::GimbalFlipThresholdDeg ||
                        std::abs(ctrlYawDelta) > Config::GimbalFlipThresholdDeg ||
                        std::abs(pitchOffset)  > Config::GimbalFlipThresholdDeg)
                    {
                        RCSDesiredPitch  = ctrlPitch;
                        RCSDesiredYaw    = ctrlYaw;
                        RCSPrevCtrlPitch = ctrlPitch;
                        RCSPrevCtrlYaw   = ctrlYaw;
                        return;
                    }

                    // ── Aimbot integration ─────────────────────────────────
                    // If the aimbot key is held, find a target and override the
                    // desired rotation with the target's look-at angles. The
                    // recoil compensation below then pre-subtracts the spring
                    // offset from THAT direction so the camera lands exactly
                    // on the target — no fight between aimbot and RCS.
                    //
                    // Silent aim takes precedence: when it's on, bullets get
                    // redirected at fire time (OnWeaponFired hook) and the camera
                    // should stay where the player aims it — so skip the snap.
                    bool aimbotSet = false;
                    if (AimbotKeyHeld && !SilentAimEnabled)
                    {
                        static constexpr float kDeg2Rad = 3.14159265f / 180.f;

                        const FVector CamLoc = CamMgr->CameraCachePrivate.POV.Location;
                        const float   PR = camRot.Pitch * kDeg2Rad;
                        const float   YR = camRot.Yaw * kDeg2Rad;
                        const FVector Forward{
                            std::cos(PR) * std::cos(YR),
                            std::cos(PR) * std::sin(YR),
                            std::sin(PR)
                        };
                        SDK::AItem* equipped = Player->InventoryComponent
                            ? Player->InventoryComponent->GetEquippedItem() : nullptr;
                        const float fov = Config::ResolveAimbotFOV(equipped);
                        if (auto target = FindAimbotTargetPos(Player, CamMgr, CamLoc, Forward, fov))
                        {
                            const FRotator AimRot = SDK::UKismetMathLibrary::MakeRotFromX(*target - CamLoc);
                            RCSDesiredPitch = std::clamp(normP(AimRot.Pitch), -90.f, 90.f);
                            RCSDesiredYaw   = normP(AimRot.Yaw);
                            aimbotSet = true;
                        }
                    }

                    if (!aimbotSet)
                    {
                        // No aimbot override — fold in actual mouse input.
                        RCSDesiredPitch += ctrlPitchDelta;
                        RCSDesiredPitch = std::clamp(RCSDesiredPitch, -90.f, 90.f);
                        RCSDesiredYaw  += ctrlYawDelta;
                        RCSDesiredYaw   = normP(RCSDesiredYaw);
                    }

                    // ── Recoil compensation (always) ───────────────────────
                    // ctrl = desired - recoil_offset  → camera = ctrl + offset = desired.
                    const float newPitch = std::clamp(
                        RCSDesiredPitch - pitchOffset * RCSFactor, -90.f, 90.f);
                    const float newYaw   = normP(
                        RCSDesiredYaw   - yawOffset   * RCSFactor);

                    FRotator rot = ctrlRot;
                    rot.Pitch = newPitch;
                    rot.Yaw   = newYaw;
                    Ctrl->SetControlRotation(rot);
                    RCSPrevCtrlPitch = newPitch;
                    RCSPrevCtrlYaw   = newYaw;
                },
                ExecutionTiming::After);
        }
        else if (RCSHandle != 0)
        {
            EngineTickHook::Get().RemoveCallback(RCSHandle);
            RCSHandle = 0;
        }
        info("[recoil] {}", RecoilEnabled ? "ON" : "OFF");
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Silent aim — redirects bullets to nearest weakpoint without moving the
    //  camera. Three hitscan paths + one projectile path:
    //    • Hitscan (a) → OnWeaponFired (AAmmoDrivenWeapon) Before, overwrites
    //                    Location in the PE-frame; native Fire reads it back
    //                    when dispatching Server_RegisterHit_*. Also stashes
    //                    target mesh + WP phys mat into g_PendingRedirect.
    //    • Hitscan (b) → Server_RegisterHit_Terrain (UHitscanComponent) Before
    //                    with SkipOriginal: when the native trace missed the
    //                    enemy and queued a terrain RPC, we swap it for the
    //                    enemy variant so the server applies damage.
    //    • Hitscan (c) → Server_RegisterHit Before with CallOriginal: when the
    //                    native trace landed on an enemy, rewrite Location and
    //                    PhysMaterial to the best-multiplier body on that same
    //                    enemy (covers weakpoints AND non-WP bonus zones). The
    //                    OnWeaponFired override already set Location, but the
    //                    PhysMaterial param is set independently by the native
    //                    code from the trace's impact mat — without this hook,
    //                    server-side damage is computed from the lower-tier
    //                    mat even when Location is at the WP.
    //    • Projectile  → Server_Fire (UProjectileLauncherBaseComponent) Before,
    //                    overwrites the FTransform rotation quaternion.
    // ─────────────────────────────────────────────────────────────────────────

    static void EnableProjectileSilentAim();
    static void DisableProjectileSilentAim();
    static void EnableHitscanSilentAim();
    static void DisableHitscanSilentAim();
    static void EnableHitscanRpcRedirect();
    static void DisableHitscanRpcRedirect();
    static void EnableHitscanHitOptimize();
    static void DisableHitscanHitOptimize();

    void ToggleSilentAim()
    {
        SilentAimEnabled = !SilentAimEnabled;

        if (SilentAimEnabled)
        {
            EnableHitscanSilentAim();
            EnableHitscanRpcRedirect();
            EnableHitscanHitOptimize();
            EnableProjectileSilentAim();
        }
        else
        {
            DisableHitscanSilentAim();
            DisableHitscanRpcRedirect();
            DisableHitscanHitOptimize();
            DisableProjectileSilentAim();
        }

        info("[silent aim] {}", SilentAimEnabled ? "ON" : "OFF");
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Projectile silent aim — SDK::UProjectileLauncherBaseComponent::Server_Fire
    //
    //  SDK::UWeaponFireComponent::Fire is a SDK::UFunction but the game's native code
    //  path invokes the C++ implementation directly, bypassing ProcessEvent —
    //  so our PE hook on it never fires. Server_Fire on the projectile-launcher
    //  subclass IS a server RPC, which DOES go through PE dispatch, so we can
    //  intercept it here. This handles grenades / launcher projectiles only;
    //  hitscan weapons need a separate native hook (see `firespy` to diagnose).
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        CallbackHandle ProjectileSilentAimHandle = 0;
    }

    static void EnableProjectileSilentAim()
    {
        if (ProjectileSilentAimHandle) return;
        ProjectileSilentAimHandle = OnProcessEventByNameAndClass(
            "Server_Fire", SDK::UProjectileLauncherBaseComponent::StaticClass(),
            [](SDK::UObject* Obj, SDK::UFunction*, void* Parms) {
                if (!Parms) return;

                auto* Launcher = Cast<SDK::UProjectileLauncherBaseComponent>(Obj);
                if (!IsValidOf<SDK::UProjectileLauncherBaseComponent>(Launcher)) return;

                SDK::APlayerCharacter* Player = GetLocalPlayer();
                if (!IsValidOf<SDK::APlayerCharacter>(Player)) return;

                // Owner of the component is the weapon actor.
                auto* OwnerWeapon = Cast<SDK::AAmmoDrivenWeapon>(Obj->Outer);
                if (!OwnerWeapon || OwnerWeapon->Character != Player || !OwnerWeapon->IsEquipped) return;

                SDK::AFSDPlayerController* Ctrl = Cast<SDK::AFSDPlayerController>(Player->Controller);
                if (!IsValidOf<SDK::AFSDPlayerController>(Ctrl)) return;
                SDK::APlayerCameraManager* CamMgr = Ctrl->PlayerCameraManager;
                if (!CamMgr) return;
                if (IsOnSpaceRig()) return;

                static constexpr float kDeg2Rad = 3.14159265f / 180.f;
                const FVector  CamLoc = CamMgr->CameraCachePrivate.POV.Location;
                const FRotator CamRot = CamMgr->CameraCachePrivate.POV.Rotation;
                const float PR = CamRot.Pitch * kDeg2Rad;
                const float YR = CamRot.Yaw   * kDeg2Rad;
                const FVector Forward{
                    std::cos(PR) * std::cos(YR),
                    std::cos(PR) * std::sin(YR),
                    std::sin(PR)
                };

                SDK::AItem* equipped = Cast<SDK::AItem>(OwnerWeapon);
                auto target = FindAimbotTargetPos(Player, CamMgr, CamLoc, Forward,
                    Config::ResolveSilentAimFOV(equipped),
                    Config::ResolveSilentAimRequireLOS(equipped));
                if (!target) return;

                // Params layout: Server_Fire(FTransform Transform, SDK::FVector_NetQuantizeNormal initialBonusVelocity,
                //                            SDK::AProjectileBase* DormentProjectile, bool notifyClients).
                // FTransform begins at +0x00; rotation is FQuat at +0x00 of the transform (16 bytes),
                // translation FVector at +0x10 (12 bytes), scale FVector at +0x20.
                // We rebuild the rotation quaternion from a desired forward direction.
                auto* parms = static_cast<uint8*>(Parms);
                FVector* trans  = reinterpret_cast<FVector*>(parms + 0x10);
                FVector  Dir    = *target - *trans;
                const float Dist = std::sqrt(Dir.X*Dir.X + Dir.Y*Dir.Y + Dir.Z*Dir.Z);
                if (Dist < 1.f) return;
                Dir.X /= Dist; Dir.Y /= Dist; Dir.Z /= Dist;

                // Quaternion from forward vector (forward = +X in UE convention).
                // axis = X cross Dir; sin/cos derived from dot.
                const float dotF  = Dir.X;                        // dot of (1,0,0) and Dir
                FVector axis{ 0.f, -Dir.Z, Dir.Y };               // cross((1,0,0), Dir) for the rotation
                const float aLen = std::sqrt(axis.X*axis.X + axis.Y*axis.Y + axis.Z*axis.Z);
                struct FQuat { float X, Y, Z, W; } q{ 0, 0, 0, 1 };
                if (aLen > 1e-6f) {
                    const float half  = 0.5f * std::acos(std::clamp(dotF, -1.f, 1.f));
                    const float sH    = std::sin(half) / aLen;
                    q.X = axis.X * sH;
                    q.Y = axis.Y * sH;
                    q.Z = axis.Z * sH;
                    q.W = std::cos(half);
                }
                // Write the rotation in-place at offset 0 of FTransform.
                *reinterpret_cast<FQuat*>(parms) = q;
            },
            ClassMatchMode::ExactOrSubclass,
            ExecutionTiming::Before,
            ExecutionMode::CallOriginal);
    }

    static void DisableProjectileSilentAim()
    {
        if (!ProjectileSilentAimHandle) return;
        RemoveHook(ProjectileSilentAimHandle);
        ProjectileSilentAimHandle = 0;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Hitscan silent aim — hook AAmmoDrivenWeapon::OnWeaponFired Before and
    //  overwrite Location with the chosen target's world position.
    //
    //  OnWeaponFired's Location is declared `const FVector&` (ConstParm|OutParm),
    //  so UE's ProcessEvent copy-back skips it — but native WeaponFireComponent::Fire
    //  re-reads the PE-frame struct after dispatch when feeding the Server_RegisterHit_*
    //  RPCs, so writing to p->Location here redirects the actual hit registration
    //  (confirmed empirically). No need to spam Fire from the tick loop anymore.
    // ─────────────────────────────────────────────────────────────────────────

    static void EnableHitscanSilentAim()
    {
        if (HitscanSilentAimHandle) return;
        HitscanSilentAimHandle = OnProcessEventByNameAndClass(
            "OnWeaponFired", SDK::AAmmoDrivenWeapon::StaticClass(),
            [](SDK::UObject* Obj, SDK::UFunction*, void* Parms) {
                if (!Parms) return;

                auto* Weapon = Cast<SDK::AAmmoDrivenWeapon>(Obj);
                if (!IsValidOf<SDK::AAmmoDrivenWeapon>(Weapon)) return;

                SDK::APlayerCharacter* Player = GetLocalPlayer();
                if (!IsValidOf<SDK::APlayerCharacter>(Player)) return;
                if (Weapon->Character != Player || !Weapon->IsEquipped) return;

                SDK::AFSDPlayerController* Ctrl = Cast<SDK::AFSDPlayerController>(Player->Controller);
                if (!IsValidOf<SDK::AFSDPlayerController>(Ctrl)) return;
                SDK::APlayerCameraManager* CamMgr = Ctrl->PlayerCameraManager;
                if (!CamMgr) return;
                if (IsOnSpaceRig()) return;

                static constexpr float kDeg2Rad = 3.14159265f / 180.f;
                const FVector  CamLoc = CamMgr->CameraCachePrivate.POV.Location;
                const FRotator CamRot = CamMgr->CameraCachePrivate.POV.Rotation;
                const float PR = CamRot.Pitch * kDeg2Rad;
                const float YR = CamRot.Yaw   * kDeg2Rad;
                const FVector Forward{
                    std::cos(PR) * std::cos(YR),
                    std::cos(PR) * std::sin(YR),
                    std::sin(PR)
                };

                // Always clear stale redirect state at shot start — covers the case
                // where a previous shot's _Terrain RPC never fired (sky shot, etc.).
                g_PendingRedirect = {};

                SDK::AItem* equipped = Cast<SDK::AItem>(Weapon);
                auto target = FindAimbotTarget(Player, CamMgr, CamLoc, Forward,
                    Config::ResolveSilentAimFOV(equipped),
                    Config::ResolveSilentAimRequireLOS(equipped));
                if (!target) return;

                auto* p = reinterpret_cast<SDK::Params::AmmoDrivenWeapon_OnWeaponFired*>(Parms);
                p->Location = target->pos;

                g_PendingRedirect.active  = true;
                g_PendingRedirect.target  = target->mesh;
                g_PendingRedirect.physMat = target->physMat;
            },
            ClassMatchMode::ExactOrSubclass,
            ExecutionTiming::Before,
            ExecutionMode::CallOriginal);
    }

    static void DisableHitscanSilentAim()
    {
        if (!HitscanSilentAimHandle) return;
        RemoveHook(HitscanSilentAimHandle);
        HitscanSilentAimHandle = 0;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Hitscan RPC redirect — convert Server_RegisterHit_Terrain into the
    //  enemy variant when OnWeaponFired flagged this shot as redirected.
    //
    //  The native trace picked _Terrain based on the player's actual aim
    //  (terrain along the view ray). Our Location override only changes the
    //  payload; the server-side handler for _Terrain ignores the enemy at
    //  that position, so damage doesn't land. We skip the original RPC and
    //  fire Server_RegisterHit with the stashed mesh + phys mat instead.
    // ─────────────────────────────────────────────────────────────────────────

    static void EnableHitscanRpcRedirect()
    {
        if (HitscanRpcRedirectHandle) return;
        HitscanRpcRedirectHandle = OnProcessEventByNameAndClass(
            "Server_RegisterHit_Terrain", SDK::UHitscanComponent::StaticClass(),
            [](SDK::UObject* Obj, SDK::UFunction*, void* Parms) {
                auto* comp = Cast<SDK::UHitscanComponent>(Obj);
                auto* tp   = Parms
                    ? reinterpret_cast<SDK::Params::HitscanComponent_Server_RegisterHit_Terrain*>(Parms)
                    : nullptr;

                bool redirected = false;
                if (comp && tp && g_PendingRedirect.active && g_PendingRedirect.target)
                {
                    comp->Server_RegisterHit(
                        tp->Location, tp->Normal,
                        g_PendingRedirect.target,
                        g_PendingRedirect.physMat);
                    redirected = true;
                }

                // Always pass-through when we didn't redirect — covers the
                // "no enemy in FOV", "redirect target was null", "cast/parm
                // validation failed" cases. Without this, those shots silently
                // void (no terrain decal, no hit sound, no surface query)
                // because SkipOriginal suppressed the engine's own dispatch.
                // The PE re-entrancy guard inside HookedProcessEvent routes
                // this re-fire straight to OriginalProcessEvent without
                // retriggering our callback.
                if (!redirected && comp && tp)
                    comp->Server_RegisterHit_Terrain(tp->Location, tp->Normal, tp->PhysMaterial);

                g_PendingRedirect = {};
            },
            ClassMatchMode::ExactOrSubclass,
            ExecutionTiming::Before,
            ExecutionMode::SkipOriginal);
    }

    static void DisableHitscanRpcRedirect()
    {
        if (!HitscanRpcRedirectHandle) return;
        RemoveHook(HitscanRpcRedirectHandle);
        HitscanRpcRedirectHandle = 0;
        g_PendingRedirect = {};
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Hitscan hit-location optimizer — Server_RegisterHit Before w/ CallOriginal.
    //
    //  When the native trace lands on an enemy, rewrite Location + PhysMaterial
    //  to the body part on THAT SAME enemy with the highest damage multiplier
    //  (weakpoint OR any high-multiplier phys mat; some bonus-damage zones
    //  aren't flagged IsWeakPoint). Pass-through unchanged if no body part
    //  has a strictly higher multiplier than what the native trace picked.
    //  Target stays the same (the actually-hit primitive component) so the
    //  server's authority check is satisfied.
    // ─────────────────────────────────────────────────────────────────────────

    static void EnableHitscanHitOptimize()
    {
        if (HitscanHitOptimizeHandle) return;
        HitscanHitOptimizeHandle = OnProcessEventByNameAndClass(
            "Server_RegisterHit", SDK::UHitscanComponent::StaticClass(),
            [](SDK::UObject* /*Obj*/, SDK::UFunction*, void* Parms) {
                if (!Parms) return;
                auto* p = reinterpret_cast<SDK::Params::HitscanComponent_Server_RegisterHit*>(Parms);
                if (!p->Target) return;

                // Hit target's owning actor — usually the enemy pawn.
                auto* Enemy = Cast<SDK::AFSDPawn>(p->Target->GetOwner());
                if (!Enemy) return;

                // Baseline = whatever multiplier the native trace's mat had,
                // or 1.0 when no phys mat was sent. FindBestDamageBody only
                // returns a result if it can beat this strictly.
                const float baseline = (p->PhysMaterial)
                    ? p->PhysMaterial->DamageMultiplier
                    : 1.f;

                auto best = FindBestDamageBody(Enemy, baseline);
                if (!best) return;   // nothing better — original hit passes through unchanged

                p->Location.X   = best->pos.X;
                p->Location.Y   = best->pos.Y;
                p->Location.Z   = best->pos.Z;
                p->PhysMaterial = best->physMat;
                // Target stays as-is.
            },
            ClassMatchMode::ExactOrSubclass,
            ExecutionTiming::Before,
            ExecutionMode::CallOriginal);
    }

    static void DisableHitscanHitOptimize()
    {
        if (!HitscanHitOptimizeHandle) return;
        RemoveHook(HitscanHitOptimizeHandle);
        HitscanHitOptimizeHandle = 0;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  firespy — diagnostic. Logs every SDK::UFunction dispatched via ProcessEvent
    //  on the local player's equipped weapon (and its components) until toggled
    //  off. Use this to discover which UFunctions actually fire during a shot
    //  so we can pick the right hook target for hitscan weapons.
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        CallbackHandle FireSpyHandle  = 0;
        bool           FireSpyEnabled = false;
    }

    void ToggleFireSpy()
    {
        FireSpyEnabled = !FireSpyEnabled;
        if (FireSpyEnabled)
        {
            FireSpyHandle = OnProcessEventAll(
                [](SDK::UObject* Obj, SDK::UFunction* Fn, void* /*Parms*/) {
                    if (!Obj || !Fn) return;
                    if (Fn->GetName() == "GetComponentByClass") return; // spammy, no useful info
                    SDK::APlayerCharacter* Player = GetLocalPlayer();
                    if (!IsValidOf<SDK::APlayerCharacter>(Player)) return;

                    // Is `Obj` the local player's equipped weapon, or a component owned by it?
                    SDK::AItem* Equipped = Player->InventoryComponent
                        ? Player->InventoryComponent->GetEquippedItem()
                        : nullptr;
                    if (!Equipped) return;

                    bool match = false;
                    if (Obj == Equipped) match = true;
                    else if (auto* outerActor = Cast<SDK::AItem>(Obj->Outer);
                             outerActor && outerActor == Equipped) match = true;

                    if (!match) return;
                    info("[firespy] {}::{} (Obj={:p})",
                        Obj->Class ? Obj->Class->GetName() : "?",
                        Fn->GetName(),
                        static_cast<void*>(Obj));
                },
                ExecutionTiming::Before,
                ExecutionMode::CallOriginal);
        }
        else
        {
            RemoveHook(FireSpyHandle);
            FireSpyHandle = 0;
        }
        info("[firespy] {}", FireSpyEnabled ? "ON" : "OFF");
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  wpinfo debug command — dumps weakpoint physics bodies for the nearest enemy.
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        void WPInfo(const CommandContext& /*ctx*/)
        {
            if (IsOnSpaceRig()) { info("[wpinfo] on space rig"); return; }

            std::vector<SDK::AFSDPawn*> Enemies = GetAliveNonFriendlies();
            if (Enemies.empty()) { info("[wpinfo] no enemies"); return; }

            SDK::APlayerCharacter* LocalPlayer = GetLocalPlayer();
            FVector CamLoc = {};
            if (IsValidOf<SDK::APlayerCharacter>(LocalPlayer))
                if (auto* Ctrl = Cast<SDK::AFSDPlayerController>(LocalPlayer->Controller))
                    if (auto* CamMgr = Ctrl->PlayerCameraManager)
                        CamLoc = CamMgr->CameraCachePrivate.POV.Location;

            SDK::AFSDPawn* Target = nullptr;
            float BestDist = FLT_MAX;
            for (SDK::AFSDPawn* E : Enemies) {
                if (!IsValidOf<SDK::AFSDPawn>(E)) continue;
                FVector D = E->K2_GetActorLocation() - CamLoc;
                float Dist = D.X * D.X + D.Y * D.Y + D.Z * D.Z;
                if (Dist < BestDist) { BestDist = Dist; Target = E; }
            }
            if (!Target) { info("[wpinfo] no valid target"); return; }

            const char* Branch = Cast<SDK::AEnemyDeepPathfinderCharacter>(Target) ? "DeepPathfinder"
                : Cast<SDK::AEnemyPawn>(Target) ? "EnemyPawn"
                : "unknown";
            auto Meshes = GetEnemyMeshes(Target);
            info("[wpinfo] branch={} meshes={}", Branch, Meshes.size());

            for (int32 m = 0; m < (int32)Meshes.size(); ++m)
            {
                auto* Mesh = Meshes[m];
                auto* SkelAsset = Mesh->SkeletalMesh;
                auto* PhysAsset = SkelAsset ? SkelAsset->PhysicsAsset : nullptr;
                info("[wpinfo] mesh[{}] bones={} skelMesh={} physAsset={} bodies={}",
                    m, Mesh->GetNumBones(),
                    SkelAsset ? "ok" : "NULL", PhysAsset ? "ok" : "NULL",
                    PhysAsset ? PhysAsset->SkeletalBodySetups.Num() : 0);
                if (!PhysAsset) continue;

                for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); ++i) {
                    auto* Body = PhysAsset->SkeletalBodySetups[i];
                    if (!Body) continue;
                    auto* FSDMat = Body->PhysMaterial ? Cast<SDK::UFSDPhysicalMaterial>(Body->PhysMaterial) : nullptr;
                    if (!FSDMat || !FSDMat->IsWeakPoint) continue;

                    const int32   BoneIdx = Mesh->GetBoneIndex(Body->BoneName);
                    const FVector Pos = Mesh->GetSocketLocation(Body->BoneName);
                    const bool    bVis = IsValidOf<SDK::APlayerCharacter>(LocalPlayer)
                        ? IsWeakpointVisible(LocalPlayer, Target, CamLoc, Pos, Body->BoneName)
                        : false;
                    const auto WpState = GetBreakableWpState(Target, Mesh, Body->BoneName, FSDMat);

                    if (WpState.isBreakable)
                    {
                        if (WpState.health >= 0.f)
                            info("[wpinfo]  WP[{}] bone='{}' boneIdx={} mult={:.2f} {} breakable hp={:.1f}{}",
                                i, Body->BoneName.ToString(), BoneIdx, FSDMat->DamageMultiplier,
                                bVis ? "VIS" : "OCC", WpState.health,
                                WpState.isDestroyed ? " DESTROYED" : "");
                        else
                            info("[wpinfo]  WP[{}] bone='{}' boneIdx={} mult={:.2f} {} breakable{}",
                                i, Body->BoneName.ToString(), BoneIdx, FSDMat->DamageMultiplier,
                                bVis ? "VIS" : "OCC",
                                WpState.isDestroyed ? " DESTROYED" : "");
                    }
                    else
                    {
                        info("[wpinfo]  WP[{}] bone='{}' boneIdx={} mult={:.2f} {} pos=({:.0f},{:.0f},{:.0f})",
                            i, Body->BoneName.ToString(), BoneIdx, FSDMat->DamageMultiplier,
                            bVis ? "VIS" : "OCC",
                            Pos.X, Pos.Y, Pos.Z);
                    }
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Registration
    // ─────────────────────────────────────────────────────────────────────────

    void RegisterCommands(CommandHandler& handler)
    {
        handler.Register("recoil",
            [](const CommandContext&) { ToggleRecoilControl(); },
            "Player",
            R"(Toggle unified RCS+aimbot loop. Aimbot fires on MouseLeft Held.)");

        handler.Register("rcsfactor",
            [](const CommandContext& ctx) {
                if (ctx.ArgCount() < 1) {
                    info("[rcsfactor] current = {:.2f}", RCSFactor);
                    return;
                }
                const float v = SafeStof(ctx.Arg(0));
                RCSFactor = std::clamp(v, 0.f, 2.f);
                info("[rcsfactor] set to {:.2f}", RCSFactor);
            },
            "Player",
            R"(Get/set RCS compensation factor 0.0-2.0 (0=off, 1=full, >1=over-correct): rcsfactor [value])");

        handler.Register("silentaim",
            [](const CommandContext&) { ToggleSilentAim(); },
            "Player",
            R"(Toggle silent aim (redirects Fire direction to nearest weakpoint, no view rotation) [Ctrl+S])");

        handler.Register("wpinfo", WPInfo, "Enemies",
            R"(Dump weakpoint physics bodies for the nearest enemy)");

        handler.Register("firespy",
            [](const CommandContext&) { ToggleFireSpy(); },
            "Player",
            R"(Toggle: log every SDK::UFunction dispatched via PE on the equipped weapon. Use to find what fires during a shot.)");
    }

    void RegisterKeybinds()
    {
        using enum Key;
        using enum Trigger;
        using enum Focus;

        KeyBindings::RegisterGameThread(MouseLeft, Mod::None,
            AimbotPressed, BindingOptions{ Press,   Game, false });
        KeyBindings::RegisterGameThread(MouseLeft, Mod::None,
            AimbotReleased, BindingOptions{ Release, Game, false });

        KeyBindings::RegisterGameThread(Key::R, Mod::Ctrl,
            ToggleRecoilControl, BindingOptions{ Press, Game, false });
        KeyBindings::RegisterGameThread(Key::S, Mod::Ctrl,
            ToggleSilentAim, BindingOptions{ Press, Game, false });
    }
} // namespace AimAssist