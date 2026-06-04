// Aimbot_TargetSelection.cpp — target finding, weakpoint scoring, and selector registry.
//
// Owns:
//   • Targeting selector registry (built-ins: default, closest, closest-wp) and
//     the Targeting::Register / Get API for custom runtime selectors.
//   • Enemy mesh traversal, breakable-armor state queries, WP visibility traces.
//   • GetWeakpointTarget — best visible WP across all meshes on one enemy.
//   • FindBestDamageBody — highest-multiplier phys body on an enemy (for RPC rewrite).
//   • FindAimbotTarget / FindAimbotTargetPos — full candidate scan + selector dispatch.

#include "Aim_Internal.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "SDK/SDK/CoreUObject_classes.hpp"
#include "SDK/SDK/Engine_classes.hpp"
#include "SDK/SDK/FSD_classes.hpp"

using namespace SDK;
using namespace ObjectCast;
using namespace GameHooks;

namespace AimAssist
{
    // ─────────────────────────────────────────────────────────────────────────
    //  Targeting — built-in selectors + registry
    // ─────────────────────────────────────────────────────────────────────────

    namespace Targeting
    {
        // Defined first so anonymous-namespace functions below can call it.
        std::unordered_map<std::string, SelectorFn>& Registry()
        {
            static std::unordered_map<std::string, SelectorFn> map;
            return map;
        }

        namespace
        {
            // ── Built-in: 5-tier priority (the documented default rule) ──────
            //   1: nearCrosshair && bodyVisible && wpVisible
            //   2: nearCrosshair && bodyVisible && hasWP && !wpVisible
            //   3: nearCrosshair && bodyVisible && !hasWP
            //   4: otherwise && hasWP        (sorted by closest)
            //   5: otherwise && !hasWP       (sorted by closest)
            // Tier 1-3 tiebreak by dot; tier 4-5 by distance.
            AFSDPawn* DefaultPriority(const std::vector<TargetCandidate>& cs)
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
                AFSDPawn* best = nullptr;
                int   bestTier = INT_MAX;
                float bestDot  = -2.f;
                float bestDist = FLT_MAX;
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

            // ── Built-in: closest enemy regardless of FOV/LOS ────────────────
            AFSDPawn* ClosestAny(const std::vector<TargetCandidate>& cs)
            {
                AFSDPawn* best = nullptr; float bestDist = FLT_MAX;
                for (const auto& c : cs)
                    if (c.distSq < bestDist) { best = c.enemy; bestDist = c.distSq; }
                return best;
            }

            // ── Built-in: closest with a weakpoint, falling back to any ──────
            AFSDPawn* ClosestWithWP(const std::vector<TargetCandidate>& cs)
            {
                AFSDPawn* best = nullptr; float bestDist = FLT_MAX;
                for (const auto& c : cs)
                {
                    if (!c.hasWP) continue;
                    if (c.distSq < bestDist) { best = c.enemy; bestDist = c.distSq; }
                }
                return best ? best : ClosestAny(cs);
            }

            void RegisterBuiltinsOnce()
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
        } // anonymous namespace

        void Register(const std::string& name, SelectorFn fn)
        {
            Registry()[name] = std::move(fn);
        }

        SelectorFn Get(const std::string& name)
        {
            RegisterBuiltinsOnce();
            auto& r = Registry();
            if (auto it = r.find(name); it != r.end()) return it->second;
            warn("[targeting] unknown selector '{}', using 'default'", name);
            return DefaultPriority;
        }
    } // namespace Targeting

    // ─────────────────────────────────────────────────────────────────────────
    //  Enemy mesh helpers
    // ─────────────────────────────────────────────────────────────────────────

    // GetMesh() on AEnemyPawn is a BlueprintImplementableEvent — always null in C++.
    // Use K2_GetComponentsByClass for AEnemyPawn; direct field for ADeepPathfinderCharacter.
    std::vector<USkeletalMeshComponent*> GetEnemyMeshes(AFSDPawn* Enemy)
    {
        if (auto* dp = Cast<AEnemyDeepPathfinderCharacter>(Enemy))
            return dp->Mesh ? std::vector<USkeletalMeshComponent*>{ dp->Mesh }
                            : std::vector<USkeletalMeshComponent*>{};

        std::vector<USkeletalMeshComponent*> result;
        auto comps = Enemy->K2_GetComponentsByClass(USkeletalMeshComponent::StaticClass());
        for (int32 i = 0; i < comps.Num(); ++i)
            if (auto* smc = Cast<USkeletalMeshComponent>(comps[i]))
                result.push_back(smc);
        return result;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Visibility traces
    // ─────────────────────────────────────────────────────────────────────────

    // Returns true if the weakpoint physics body is visible from CamLoc.
    // Uses physics-body trace (bTraceComplex=false) so FHitResult::BoneName is
    // populated — direct name match beats a fragile distance threshold.
    bool IsWeakpointVisible(APlayerCharacter* LocalPlayer, AFSDPawn* Enemy,
        const FVector& CamLoc, const FVector& WPos, FName BoneName)
    {
        TArray<AActor*> NoIgnore;
        FHitResult Hit;
        const bool bHit = UKismetSystemLibrary::LineTraceSingle(
            LocalPlayer, CamLoc, WPos,
            ETraceTypeQuery::TraceTypeQuery1, false,
            NoIgnore, EDrawDebugTrace::None,
            &Hit, true,
            FLinearColor{}, FLinearColor{}, 0.f);

        if (!bHit) return true;
        if (Hit.Actor.Get() != static_cast<AActor*>(Enemy)) return false;
        return Hit.BoneName == BoneName;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Breakable weakpoint state
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        // UE4 4.27 TMap AllocationFlags: inline uint32[4] starts at map_base+0x10.
        bool TMapSlotValid(const void* m, int32 i)
        {
            if (i < 0 || i >= 128) return false;
            const auto* f = reinterpret_cast<const uint32*>(static_cast<const uint8*>(m) + 0x10);
            return (f[i >> 5] >> (i & 31)) & 1u;
        }
    }

    // TMap<FName(8), Value(0x18)> stride: sizeof(TSetElement<TPair<K,V>>) = 40 bytes.
    //   slot+0x00: FName key   slot+0x08: Value (24 bytes)
    //   slot+0x20: HashNextId  slot+0x24: HashIndex
    BreakableWpState GetBreakableWpState(AFSDPawn* Enemy, USkeletalMeshComponent* Mesh,
        FName BoneName, UFSDPhysicalMaterial* PhysMat)
    {
        BreakableWpState r;

        auto comps = Enemy->K2_GetComponentsByClass(UBaseArmorDamageComponent::StaticClass());
        for (int32 ci = 0; ci < comps.Num(); ++ci)
        {
            auto* base = Cast<UBaseArmorDamageComponent>(comps[ci]);
            if (!base || base->Mesh != Mesh) continue;

            bool manages = false;
            for (int32 mi = 0; mi < base->ArmorPhysMats.Num(); ++mi)
                if (base->ArmorPhysMats[mi] == PhysMat) { manages = true; break; }
            if (!manages) continue;

            r.isBreakable = true;

            if (auto* hc = Cast<UArmorHealthDamageComponent>(base))
            {
                const void* mp = &hc->PhysBoneToArmor;
                const int32  nSlots = *reinterpret_cast<const int32*>(static_cast<const uint8*>(mp) + 0x08);
                const uint8* buf = *reinterpret_cast<uint8* const*>(mp);
                if (!buf) break;

                for (int32 i = 0; i < nSlots; ++i)
                {
                    if (!TMapSlotValid(mp, i)) continue;
                    const uint8* slot = buf + i * 40;
                    const FArmorHealthItem* item = reinterpret_cast<const FArmorHealthItem*>(slot + 8);
                    const bool byMask = (hc->ArmorDamageInfo.ArmorIndexMask >> item->MaterialIndex) & 1;

                    for (int32 bi = 0; bi < item->ArmorBones.Num(); ++bi)
                    {
                        const FArmorHealthSubItem& sub = item->ArmorBones[bi];
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
            else if (auto* sc = Cast<USimpleArmorDamageComponent>(base))
            {
                const void* mp = &sc->PhysBoneToArmor;
                const int32  nSlots = *reinterpret_cast<const int32*>(static_cast<const uint8*>(mp) + 0x08);
                const uint8* buf = *reinterpret_cast<uint8* const*>(mp);
                if (!buf) break;

                for (int32 i = 0; i < nSlots; ++i)
                {
                    if (!TMapSlotValid(mp, i)) continue;
                    const uint8* slot = buf + i * 40;
                    const FDestructableBodypartItem* item = reinterpret_cast<const FDestructableBodypartItem*>(slot + 8);

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

    // ─────────────────────────────────────────────────────────────────────────
    //  File-local helpers (only called from functions in this TU)
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        // Largest sphere/capsule radius from the body's aggregate geometry,
        // scaled by Config::BodyRadiusScale; falls back to Config::BodyRadiusFallback.
        float GetBodyRadius(USkeletalBodySetup* Body)
        {
            float r = 0.f;
            for (int32 i = 0; i < Body->AggGeom.SphereElems.Num(); ++i)
                r = std::max(r, Body->AggGeom.SphereElems[i].Radius);
            for (int32 i = 0; i < Body->AggGeom.SphylElems.Num(); ++i)
                r = std::max(r, Body->AggGeom.SphylElems[i].Radius);
            auto g = Config::GetGlobals();
            return r > 0.f ? r * g->BodyRadiusScale : g->BodyRadiusFallback;
        }

        struct FDamageInfo
        {
            float Damage = 0.f;
            float RadialDamage = 0.f;
            bool IsValid() const { return Damage > 0.001f || RadialDamage > 0.01f; }
        };

        bool ExtractDamageInfo(UDamageComponent* DamageComponent, FDamageInfo& OutInfo)
        {
            if (!DamageComponent) return false;
            OutInfo.Damage = DamageComponent->Damage;
            OutInfo.RadialDamage = DamageComponent->RadialDamage;
            return true;
        }

        bool GetDamageInfoFromProjectileClass(UClass* ProjectileClass, FDamageInfo& OutInfo)
        {
            if (!ProjectileClass) return false;
            AProjectileBase* ProjectileCDO = Cast<AProjectileBase>(ProjectileClass->ClassDefaultObject);
            if (!ProjectileCDO) return false;

            if (ExtractDamageInfo(
                Cast<UDamageComponent>(ProjectileCDO->GetComponentByClass(UDamageComponent::StaticClass())),
                OutInfo))
                return true;

            // Components created at runtime aren't on the CDO — spawn a throwaway
            // projectile far off-grid and read its damage component.
            FTransform Transform{};
            Transform.Translation = FVector{ 9999999, 9999999, 9999999 };

            AProjectileBase* Spawned = nullptr;
            if (!SpawnActor<AProjectileBase>(ProjectileClass, Transform, Spawned))
                return false;

            const bool ok = ExtractDamageInfo(
                Cast<UDamageComponent>(Spawned->GetComponentByClass(UDamageComponent::StaticClass())),
                OutInfo);
            Spawned->K2_DestroyActor();
            return ok;
        }

        // Cheap body-visibility check: line trace from camera to actor location.
        bool IsBodyVisible(APlayerCharacter* LocalPlayer, AFSDPawn* Enemy, const FVector& CamLoc)
        {
            TArray<AActor*> NoIgnore;
            FHitResult Hit;
            const FVector Target = Enemy->K2_GetActorLocation();
            const bool bHit = UKismetSystemLibrary::LineTraceSingle(
                LocalPlayer, CamLoc, Target,
                ETraceTypeQuery::TraceTypeQuery1, false,
                NoIgnore, EDrawDebugTrace::None,
                &Hit, true,
                FLinearColor{}, FLinearColor{}, 0.f);
            if (!bHit) return true;
            return Hit.Actor.Get() == static_cast<AActor*>(Enemy);
        }

        struct WpTargetResult
        {
            FVector pos = {};
            bool hasWeakpoints = false; // physics asset declares any WP body
            bool anyVisible    = false; // ≥1 visible & not destroyed
            USkeletalMeshComponent* mesh    = nullptr;  // mesh that owns the chosen WP
            UFSDPhysicalMaterial*   physMat = nullptr;  // chosen WP's phys material
        };

        WpTargetResult GetWeakpointTarget(AFSDPawn* Enemy,
            const std::vector<USkeletalMeshComponent*>& Meshes,
            const FVector& CamLoc, const FVector& Forward,
            APlayerCharacter* LocalPlayer,
            bool RequireLOS)
        {
            WpTargetResult result;
            FVector BestPos = {};
            float   BestMult = -1.f;
            float   BestDot  = -2.f;
            USkeletalMeshComponent* BestMesh    = nullptr;
            UFSDPhysicalMaterial*   BestPhysMat = nullptr;

            for (USkeletalMeshComponent* Mesh : Meshes)
            {
                if (!Mesh) continue;
                auto* SkelAsset = Mesh->SkeletalMesh;
                if (!SkelAsset) continue;
                auto* PhysAsset = SkelAsset->PhysicsAsset;
                if (!PhysAsset) continue;

                for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); ++i)
                {
                    USkeletalBodySetup* Body = PhysAsset->SkeletalBodySetups[i];
                    if (!Body || !Body->PhysMaterial) continue;

                    UFSDPhysicalMaterial* FSDMat = Cast<UFSDPhysicalMaterial>(Body->PhysMaterial);
                    if (!FSDMat || !FSDMat->IsWeakPoint) continue;

                    result.hasWeakpoints = true;

                    auto WpState = GetBreakableWpState(Enemy, Mesh, Body->BoneName, FSDMat);
                    if (WpState.isBreakable && WpState.isDestroyed) continue;

                    const FVector Center = Mesh->GetSocketLocation(Body->BoneName);
                    const float   R = GetBodyRadius(Body);

                    // VisPos defaults to bone pivot. With RequireLOS off we
                    // commit to the pivot directly — silent aim doesn't care if
                    // the player can see the WP from the camera angle.
                    FVector VisPos   = Center;
                    bool    bVisible = true;
                    if (RequireLOS)
                    {
                        bVisible = false;
                        auto cfg = Config::GetGlobals();
                        for (const auto& Off : cfg->WpSampleOffsets)
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

                    const float Dot  = Forward.X * Dir.X + Forward.Y * Dir.Y + Forward.Z * Dir.Z;
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
                result.pos     = Enemy->K2_GetActorLocation();
                result.mesh    = Meshes.empty() ? nullptr : Meshes.front();
                result.physMat = nullptr;
            }
            return result;
        }
    } // anonymous namespace

    // ─────────────────────────────────────────────────────────────────────────
    //  Best damage body — highest-multiplier phys body on an enemy.
    //  Used by the Server_RegisterHit RPC hooks to upgrade an already-hit
    //  location to a better body part (weakpoints AND non-WP bonus zones).
    // ─────────────────────────────────────────────────────────────────────────

    std::optional<BestDamageBody> FindBestDamageBody(AFSDPawn* Enemy, float MinMultiplier)
    {
        if (!Enemy) return std::nullopt;
        auto meshes = GetEnemyMeshes(Enemy);
        if (meshes.empty()) return std::nullopt;

        BestDamageBody best;
        best.multiplier = MinMultiplier;
        bool found = false;

        for (USkeletalMeshComponent* Mesh : meshes)
        {
            if (!Mesh || !Mesh->SkeletalMesh || !Mesh->SkeletalMesh->PhysicsAsset) continue;
            auto* PhysAsset = Mesh->SkeletalMesh->PhysicsAsset;
            for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); ++i)
            {
                auto* Body = PhysAsset->SkeletalBodySetups[i];
                if (!Body || !Body->PhysMaterial) continue;
                auto* M = Cast<UFSDPhysicalMaterial>(Body->PhysMaterial);
                if (!M) continue;

                // Skip destroyed breakable weakpoints — broken armor zones
                // no longer apply the multiplier server-side.
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
                    best.boneName   = Body->BoneName;
                    found = true;
                }
            }
        }
        if (found) return best;

        // Fallback for "give me anything" callers (MinMultiplier <= 0): some
        // enemies (basic grunts) have plain UPhysicalMaterial on every body —
        // the FSD cast fails and we'd return nullopt, breaking the MultiHitscan
        // redirect path. Hand back actor-center + null physMat; caller skips the
        // Materials overwrite when physMat is null but still commits HitLocation.
        if (MinMultiplier > 0.f) return std::nullopt;

        BestDamageBody fallback;
        fallback.pos        = Enemy->K2_GetActorLocation();
        fallback.mesh       = meshes.empty() ? nullptr : meshes.front();
        fallback.physMat    = nullptr;
        fallback.multiplier = 1.f;
        return fallback;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  FindAimbotTarget — full candidate scan + selector dispatch
    // ─────────────────────────────────────────────────────────────────────────

    std::optional<AimTargetInfo> FindAimbotTarget(APlayerCharacter* LocalPlayer,
        APlayerCameraManager* /*CamMgr*/,
        const FVector& CamLoc,
        const FVector& Forward,
        float FOVDeg,
        bool RequireLOS)
    {
        static constexpr float kDeg2Rad = 3.14159265f / 180.f;

        if (IsOnSpaceRig()) return std::nullopt;

        UInventoryComponent* Inventory = LocalPlayer->InventoryComponent;
        if (!Inventory) return std::nullopt;

        AItem* Equipped = Inventory->GetEquippedItem();
        if (!Equipped) return std::nullopt;

        auto cfgIgnore = Config::GetGlobals();
        for (const FName& ClassName : cfgIgnore->IgnoredItemClasses)
            if (IsChildOfByName(Equipped, ClassName)) return std::nullopt;

        FDamageInfo DamageInfo;
        if (UDamageComponent* DamageComponent = GetComponent<UDamageComponent>(Equipped))
            ExtractDamageInfo(DamageComponent, DamageInfo);
        else
        {
            UProjectileLauncherComponent* Launcher = GetComponent<UProjectileLauncherComponent>(Equipped);
            if (!Launcher) return std::nullopt;
            if (!GetDamageInfoFromProjectileClass(Launcher->ProjectileClass, DamageInfo)) return std::nullopt;
        }
        if (!DamageInfo.IsValid()) return std::nullopt;

        std::vector<AFSDPawn*> Enemies = GetAliveNonFriendlies();
        if (Enemies.empty()) return std::nullopt;

        const float HalfFOVCos = std::cos(FOVDeg * 0.5f * kDeg2Rad);

        // Lazy default-seed for IgnoreBaseClasses (from YAML or C++ fallback).
        if (IgnoreBaseClasses.empty()) [[unlikely]]
            IgnoreBaseClasses = Config::GetGlobals()->DefaultIgnoreBaseClasses;

        // Resolve effective ignore/force lists: a per-weapon override REPLACES
        // the global mutable list entirely (no merging).
        const std::vector<FName>* effIgnore = &IgnoreBaseClasses;
        const std::vector<FName>* effForce  = &ForceIncludeClasses;
        // Hold the override snapshot for the rest of the function — effIgnore /
        // effForce may point into its entries below, so it must outlive the block.
        auto overrides = Config::WeaponOverrides();
        {
            auto& m = *overrides;
            auto it = Equipped->Class ? m.find(Equipped->Class->Name.ToString()) : m.end();
            if (it != m.end())
            {
                if (it->second.IgnoreBaseClasses)   effIgnore = &*it->second.IgnoreBaseClasses;
                if (it->second.ForceIncludeClasses) effForce  = &*it->second.ForceIncludeClasses;
            }
        }

        std::vector<Targeting::TargetCandidate> candidates;
        candidates.reserve(Enemies.size());

        for (AFSDPawn* Enemy : Enemies)
        {
            if (!IsValidOf<AFSDPawn>(Enemy)) continue;

            bool forceInclude = false;
            if (Enemy->Class)
                for (const FName& name : *effForce)
                    if (Enemy->Class->Name == name) { forceInclude = true; break; }

            if (!forceInclude)
            {
                bool eliteOrBoss = Enemy->IsElite();
                if (!eliteOrBoss)
                    if (auto* hc = Cast<UEnemyHealthComponent>(Enemy->GetHealthComponent()))
                        eliteOrBoss = hc->bIsBossFight;

                if (!eliteOrBoss)
                {
                    bool ignored = false;
                    for (const FName& name : *effIgnore)
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

            auto meshes = GetEnemyMeshes(Enemy);
            for (USkeletalMeshComponent* Mesh : meshes)
            {
                if (!Mesh || !Mesh->SkeletalMesh || !Mesh->SkeletalMesh->PhysicsAsset) continue;
                auto* PhysAsset = Mesh->SkeletalMesh->PhysicsAsset;
                for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num() && !cand.hasWP; ++i)
                {
                    auto* Body = PhysAsset->SkeletalBodySetups[i];
                    if (!Body || !Body->PhysMaterial) continue;
                    auto* M = Cast<UFSDPhysicalMaterial>(Body->PhysMaterial);
                    if (!M || !M->IsWeakPoint) continue;
                    auto st = GetBreakableWpState(Enemy, Mesh, Body->BoneName, M);
                    if (st.isBreakable && st.isDestroyed) continue;
                    cand.hasWP = true;
                }
                if (cand.hasWP) break;
            }

            if (cand.nearCrosshair)
            {
                cand.bodyVisible = IsBodyVisible(LocalPlayer, Enemy, CamLoc);
                if (cand.bodyVisible && cand.hasWP)
                {
                    auto probe = GetWeakpointTarget(Enemy, meshes, CamLoc, Forward, LocalPlayer, /*RequireLOS*/true);
                    cand.wpVisible = probe.anyVisible;
                }
            }

            candidates.push_back(cand);
        }
        if (candidates.empty()) return std::nullopt;

        Targeting::SelectorFn selector = Config::ResolveTargetSelector(Equipped);
        AFSDPawn* chosen = selector(candidates);
        if (!chosen) return std::nullopt;

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

    std::optional<FVector> FindAimbotTargetPos(APlayerCharacter* LocalPlayer,
        APlayerCameraManager* CamMgr,
        const FVector& CamLoc,
        const FVector& Forward,
        float FOVDeg,
        bool RequireLOS)
    {
        auto t = FindAimbotTarget(LocalPlayer, CamMgr, CamLoc, Forward, FOVDeg, RequireLOS);
        if (!t) return std::nullopt;
        return t->pos;
    }

} // namespace AimAssist
