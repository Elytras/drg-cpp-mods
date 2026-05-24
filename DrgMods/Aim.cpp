// Aim.cpp — Aimbot + RCS + Silent Aim implementation.
// See Aim.h for the public surface.

#include "Aim.h"
#include "Library.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "SDK/SDK/Basic.hpp"
#include "SDK/SDK/CoreUObject_classes.hpp"
#include "SDK/SDK/Engine_classes.hpp"
#include "SDK/SDK/FSD_classes.hpp"
#include "SDK/SDK/FSD_parameters.hpp"

extern HANDLE g_hRespEvent;

using namespace ObjectCast;
using namespace GameHooks;

namespace AimAssist
{
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

        CallbackHandle SilentAimHandle = 0;
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
            // Direct-field access � use SDK::FTransform (our ::FTransform wrapper has accessors only).
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

        // Largest sphere/capsule radius from the body's aggregate geometry.
        // Fallback 15 UU (~15 cm) for bodies with no sphere or sphyl shapes.
        float GetBodyRadius(SDK::USkeletalBodySetup* Body)
        {
            float r = 0.f;
            for (int32 i = 0; i < Body->AggGeom.SphereElems.Num(); ++i)
                r = std::max(r, Body->AggGeom.SphereElems[i].Radius);
            for (int32 i = 0; i < Body->AggGeom.SphylElems.Num(); ++i)
                r = std::max(r, Body->AggGeom.SphylElems[i].Radius);
            return r > 0.f ? r * 0.75f : 15.f;
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
        };

        WpTargetResult GetWeakpointTarget(SDK::AFSDPawn* Enemy,
            const std::vector<SDK::USkeletalMeshComponent*>& Meshes,
            const FVector& CamLoc, const FVector& Forward,
            SDK::APlayerCharacter* LocalPlayer)
        {
            WpTargetResult result;
            FVector BestPos = {};
            float   BestMult = -1.f;
            float   BestDot = -2.f;

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

                    // Multipoint: try center then ±R on each world axis.
                    // First visible candidate wins as the aim position.
                    static const std::array<FVector, 7> kOffsets = { {
                        {0,0,0}, {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
                    } };
                    FVector VisPos = {};
                    bool    bVisible = false;
                    for (const auto& Off : kOffsets)
                    {
                        FVector Candidate = { Center.X + Off.X * R, Center.Y + Off.Y * R, Center.Z + Off.Z * R };
                        if (IsWeakpointVisible(LocalPlayer, Enemy, CamLoc, Candidate, Body->BoneName))
                        {
                            VisPos = Candidate; bVisible = true; break;
                        }
                    }
                    if (!bVisible) continue;

                    FVector Dir = VisPos - CamLoc;
                    const float Dist = std::sqrt(Dir.X * Dir.X + Dir.Y * Dir.Y + Dir.Z * Dir.Z);
                    if (Dist < 1.f) continue;
                    Dir.X /= Dist; Dir.Y /= Dist; Dir.Z /= Dist;

                    const float Dot = Forward.X * Dir.X + Forward.Y * Dir.Y + Forward.Z * Dir.Z;
                    const float Mult = FSDMat->DamageMultiplier;

                    if (Mult > BestMult || (Mult >= BestMult && Dot > BestDot))
                    {
                        BestMult = Mult; BestDot = Dot; BestPos = VisPos; result.anyVisible = true;
                    }
                }
            }

            result.pos = result.anyVisible ? BestPos : Enemy->K2_GetActorLocation();
            return result;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Best aim target across all enemies in the FOV cone
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        std::optional<FVector> FindAimbotTargetPos(SDK::APlayerCharacter* LocalPlayer,
            SDK::APlayerCameraManager* /*CamMgr*/,
            const FVector& CamLoc,
            const FVector& Forward,
            float FOVDeg)
        {
            static constexpr float kDeg2Rad = 3.14159265f / 180.f;

            if (IsOnSpaceRig()) return std::nullopt;

            SDK::UInventoryComponent* Inventory = LocalPlayer->InventoryComponent;
            if (!Inventory) return std::nullopt;

            SDK::AItem* Equipped = Inventory->GetEquippedItem();
            if (!Equipped) return std::nullopt;

            // Equipped items that aren't damage-dealing weapons — aimbot stays
            // dormant for these (mining tools, traversal gear, etc).
            static std::vector<SDK::FName> IgnoredItemClasses{};
            if (IgnoredItemClasses.empty()) [[unlikely]]
            {
                const std::vector<SDK::UClass*> Classes = {
                    SDK::APickaxeItem::StaticClass(),
                    SDK::ADoubleDrillItem::StaticClass(),
                    SDK::AZipLineItem::StaticClass(),
                    SDK::AGrapplingHookGun::StaticClass(),
                };
                for (SDK::UClass* Class : Classes)
                    if (IsValidOf<SDK::UClass>(Class))
                        IgnoredItemClasses.push_back(Class->Name);
                IgnoredItemClasses.push_back(SDK::FName(L"WPN_PlatformGun_C"));
            }
            for (const SDK::FName& ClassName : IgnoredItemClasses)
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

            // Lazy default-initializer for IgnoreBaseClasses. Users can add to
            // either list externally at any time after first call.
            if (IgnoreBaseClasses.empty()) [[unlikely]]
                IgnoreBaseClasses.push_back(SDK::FName(L"SDK::ENE_Spider_Grunt_Base_C"));

            struct Candidate { float dot; SDK::AFSDPawn* enemy; };
            std::vector<Candidate> candidates;

            for (SDK::AFSDPawn* Enemy : Enemies)
            {
                if (!IsValidOf<SDK::AFSDPawn>(Enemy)) continue;

                // Inclusion rules:
                //   1. ForceIncludeClasses (exact)         → always targeted.
                //   2. Elite / boss                        → bypass ignore list.
                //   3. IgnoreBaseClasses (subclass match)  → skipped.
                bool forceInclude = false;
                if (Enemy->Class)
                    for (const SDK::FName& name : ForceIncludeClasses)
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
                        for (const SDK::FName& name : IgnoreBaseClasses)
                            if (IsChildOfByName(Enemy, name)) { ignored = true; break; }
                        if (ignored) continue;
                    }
                }

                FVector ToEnemy = Enemy->K2_GetActorLocation() - CamLoc;
                const float Dist = std::sqrt(ToEnemy.X * ToEnemy.X + ToEnemy.Y * ToEnemy.Y + ToEnemy.Z * ToEnemy.Z);
                if (Dist < 0.01f) continue;
                ToEnemy.X /= Dist; ToEnemy.Y /= Dist; ToEnemy.Z /= Dist;
                const float Dot = Forward.X * ToEnemy.X + Forward.Y * ToEnemy.Y + Forward.Z * ToEnemy.Z;
                if (Dot >= HalfFOVCos) candidates.push_back({ Dot, Enemy });
            }
            if (candidates.empty()) return std::nullopt;
            std::sort(candidates.begin(), candidates.end(),
                [](const Candidate& a, const Candidate& b) { return a.dot > b.dot; });

            for (auto& c : candidates)
            {
                auto meshes = GetEnemyMeshes(c.enemy);
                if (meshes.empty()) return c.enemy->K2_GetActorLocation();
                auto wp = GetWeakpointTarget(c.enemy, meshes, CamLoc, Forward, LocalPlayer);
                if (wp.hasWeakpoints && !wp.anyVisible) continue;
                return wp.pos;
            }
            return std::nullopt;
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
        RecoilEnabled = !RecoilEnabled;
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

            EnqueueWhile([normP]() -> bool
                {
                    if (!RecoilEnabled) return false;

                    SDK::APlayerCharacter* Player = GetLocalPlayer();
                    if (!IsValidOf<SDK::APlayerCharacter>(Player)) return true;

                    SDK::AFSDPlayerController* Ctrl = Cast<SDK::AFSDPlayerController>(Player->Controller);
                    if (!IsValidOf<SDK::AFSDPlayerController>(Ctrl)) return true;

                    SDK::APlayerCameraManager* CamMgr = Ctrl->PlayerCameraManager;
                    if (!CamMgr) return true;

                    const FRotator ctrlRot = Ctrl->GetControlRotation();
                    const FRotator camRot = CamMgr->CameraCachePrivate.POV.Rotation;
                    const float ctrlPitch = normP(ctrlRot.Pitch);
                    const float camPitch = normP(camRot.Pitch);
                    const float ctrlYaw = normP(ctrlRot.Yaw);
                    const float camYaw = normP(camRot.Yaw);

                    // EnqueueWhile fires on every ProcessEvent — many times per
                    // rendered frame. Camera updates once per frame. Gate on
                    // cam values changing so we apply exactly one correction
                    // per camera frame, not N (which would compound to ±90).
                    static float lastCamPitch = 1e9f;
                    static float lastCamYaw = 1e9f;

                    if (!RCSInitialized)
                    {
                        lastCamPitch = camPitch;
                        lastCamYaw = camYaw;
                        RCSDesiredPitch = ctrlPitch;
                        RCSPrevCtrlPitch = ctrlPitch;
                        RCSDesiredYaw = ctrlYaw;
                        RCSPrevCtrlYaw = ctrlYaw;
                        RCSInitialized = true;
                        return true;
                    }
                    if (camPitch == lastCamPitch && camYaw == lastCamYaw) return true;
                    lastCamPitch = camPitch;
                    lastCamYaw = camYaw;

                    // ── Aimbot integration ─────────────────────────────────
                    // If the aimbot key is held, find a target and override the
                    // desired rotation with the target's look-at angles. The
                    // recoil compensation below then pre-subtracts the spring
                    // offset from THAT direction so the camera lands exactly
                    // on the target — no fight between aimbot and RCS.
                    //
                    // Silent aim takes precedence: when it's on, the whole point
                    // is that the camera stays where the player aims it while
                    // bullets get redirected on fire. Snapping the view to the
                    // target would defeat that, so skip the aimbot branch here
                    // and let the player's actual mouse input drive `desired`.
                    bool aimbotSet = false;
                    if (AimbotKeyHeld && !SilentAimEnabled)
                    {
                        static constexpr float kDeg2Rad = 3.14159265f / 180.f;
                        static constexpr float kFOV = 90.f;

                        const FVector CamLoc = CamMgr->CameraCachePrivate.POV.Location;
                        const float   PR = camRot.Pitch * kDeg2Rad;
                        const float   YR = camRot.Yaw * kDeg2Rad;
                        const FVector Forward{
                            std::cos(PR) * std::cos(YR),
                            std::cos(PR) * std::sin(YR),
                            std::sin(PR)
                        };
                        if (auto target = FindAimbotTargetPos(Player, CamMgr, CamLoc, Forward, kFOV))
                        {
                            const FRotator AimRot = SDK::UKismetMathLibrary::MakeRotFromX(*target - CamLoc);
                            RCSDesiredPitch = std::clamp(normP(AimRot.Pitch), -90.f, 90.f);
                            RCSDesiredYaw = normP(AimRot.Yaw);
                            aimbotSet = true;
                        }
                    }
                    else if (AimbotKeyHeld && SilentAimEnabled) {
                        if (Player->InventoryComponent)
                            if (SDK::UWeaponFireComponent* Component = GetComponent<SDK::UWeaponFireComponent>(Player->InventoryComponent->GetEquippedItem())) {
                                static constexpr float kDeg2Rad = 3.14159265f / 180.f;
                                static constexpr float kFOV = 90.f;

                                const FVector CamLoc = CamMgr->CameraCachePrivate.POV.Location;
                                const float   PR = camRot.Pitch * kDeg2Rad;
                                const float   YR = camRot.Yaw * kDeg2Rad;
                                const FVector Forward{
                                    std::cos(PR) * std::cos(YR),
                                    std::cos(PR) * std::sin(YR),
                                    std::sin(PR)
                                };
                                if (auto target = FindAimbotTargetPos(Player, CamMgr, CamLoc, Forward, kFOV))
                                {
                                    Component->Fire(CamLoc, SDK::FVector_NetQuantizeNormal(((*target) - CamLoc).Normalize()), false);
                                }
                            }
                    }

                    if (!aimbotSet)
                    {
                        // No aimbot override — fold in actual mouse input.
                        RCSDesiredPitch += normP(ctrlPitch - RCSPrevCtrlPitch);
                        RCSDesiredPitch = std::clamp(RCSDesiredPitch, -90.f, 90.f);
                        RCSDesiredYaw += normP(ctrlYaw - RCSPrevCtrlYaw);
                        RCSDesiredYaw = normP(RCSDesiredYaw);
                    }

                    // ── Recoil compensation (always) ───────────────────────
                    // ctrl = desired - recoil_offset  → camera = ctrl + offset = desired.
                    const float pitchOffset = camPitch - ctrlPitch;
                    const float yawOffset = normP(camYaw - ctrlYaw);
                    const float newPitch = std::clamp(
                        RCSDesiredPitch - pitchOffset * RCSFactor, -90.f, 90.f);
                    const float newYaw = normP(
                        RCSDesiredYaw - yawOffset * RCSFactor);

                    FRotator rot = ctrlRot;
                    rot.Pitch = newPitch;
                    rot.Yaw = newYaw;
                    Ctrl->SetControlRotation(rot);
                    RCSPrevCtrlPitch = newPitch;
                    RCSPrevCtrlYaw = newYaw;

                    return true;
                });
        }
        info("[recoil] {}", RecoilEnabled ? "ON" : "OFF");
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Silent aim — redirects SDK::UWeaponFireComponent::Fire's Direction param
    // ─────────────────────────────────────────────────────────────────────────

    // Forward decls — implementations live below ToggleSilentAim.
    static void EnableProjectileSilentAim();
    static void DisableProjectileSilentAim();

    void ToggleSilentAim()
    {
        SilentAimEnabled = !SilentAimEnabled;
        if (SilentAimEnabled)
        {
            SilentAimHandle = OnProcessEventByNameAndClass(
                "Fire", SDK::UWeaponFireComponent::StaticClass(),
                [](SDK::UObject* Obj, SDK::UFunction*, void* Parms) {
                    if (!Parms) return;
                    //info("SilentAim Triggered");
                    auto* Weapon = Cast<SDK::AAmmoDrivenWeapon>(Obj->Outer);
                    if (!IsValidOf<SDK::AAmmoDrivenWeapon>(Weapon)) return;

                    SDK::APlayerCharacter* Player = GetLocalPlayer();
                    if (!IsValidOf<SDK::APlayerCharacter>(Player)) return;
                    if (Weapon->Character != Player || !Weapon->IsEquipped) return;

                    SDK::AFSDPlayerController* Ctrl = Cast<SDK::AFSDPlayerController>(Player->Controller);
                    if (!IsValidOf<SDK::AFSDPlayerController>(Ctrl)) return;

                    SDK::APlayerCameraManager* CamMgr = Ctrl->PlayerCameraManager;
                    if (!CamMgr) return;

                    if (IsOnSpaceRig()) return;

                    std::vector<SDK::AFSDPawn*> Enemies = GetAliveNonFriendlies();
                    if (Enemies.empty()) return;

                    // Silent aim uses a tighter FOV than aimbot (~30°) — only
                    // redirects shots aimed near the target, never wildly off.
                    static constexpr float kDeg2Rad = 3.14159265f / 180.f;
                    static constexpr float kFOV = 30.f;

                    const FVector  CamLoc = CamMgr->GetCameraLocation();
                    const FRotator CamRot = CamMgr->GetCameraRotation();
                    const float PR = CamRot.Pitch * kDeg2Rad;
                    const float YR = CamRot.Yaw * kDeg2Rad;
                    const FVector Forward{
                        std::cos(PR) * std::cos(YR),
                        std::cos(PR) * std::sin(YR),
                        std::sin(PR)
                    };

                    const float HalfFOVCos = std::cos(kFOV * 0.5f * kDeg2Rad);

                    struct Candidate { float dot; SDK::AFSDPawn* enemy; };
                    std::vector<Candidate> candidates;
                    for (SDK::AFSDPawn* Enemy : Enemies)
                    {
                        if (!IsValidOf<SDK::AFSDPawn>(Enemy)) continue;
                        FVector ToEnemy = Enemy->K2_GetActorLocation() - CamLoc;
                        const float Dist = std::sqrt(ToEnemy.X * ToEnemy.X + ToEnemy.Y * ToEnemy.Y + ToEnemy.Z * ToEnemy.Z);
                        if (Dist < 0.01f) continue;
                        ToEnemy.X /= Dist; ToEnemy.Y /= Dist; ToEnemy.Z /= Dist;
                        const float Dot = Forward.X * ToEnemy.X + Forward.Y * ToEnemy.Y + Forward.Z * ToEnemy.Z;
                        if (Dot >= HalfFOVCos) candidates.push_back({ Dot, Enemy });
                    }
                    if (candidates.empty()) return;
                    std::sort(candidates.begin(), candidates.end(),
                        [](const Candidate& a, const Candidate& b) { return a.dot > b.dot; });

                    SDK::AFSDPawn* BestTarget = nullptr;
                    FVector   AimPos = {};
                    for (auto& c : candidates)
                    {
                        auto meshes = GetEnemyMeshes(c.enemy);
                        if (meshes.empty()) { BestTarget = c.enemy; AimPos = c.enemy->K2_GetActorLocation(); break; }
                        auto wp = GetWeakpointTarget(c.enemy, meshes, CamLoc, Forward, Player);
                        if (wp.hasWeakpoints && !wp.anyVisible) continue;
                        BestTarget = c.enemy;
                        AimPos = wp.pos;
                        break;
                    }
                    if (!BestTarget) return;

                    // Overwrite Direction in-place. Origin (barrel) stays as the
                    // weapon set it; we recompute Direction to point at the WP.
                    auto* p = reinterpret_cast<SDK::Params::WeaponFireComponent_Fire*>(Parms);
                    FVector Dir = AimPos - p->Origin;
                    const float Dist = std::sqrt(Dir.X * Dir.X + Dir.Y * Dir.Y + Dir.Z * Dir.Z);
                    if (Dist < 1.f) return;
                    Dir.X /= Dist; Dir.Y /= Dist; Dir.Z /= Dist;
                    p->Direction.X = Dir.X;
                    p->Direction.Y = Dir.Y;
                    p->Direction.Z = Dir.Z;
                },
                ClassMatchMode::ExactOrSubclass,
                ExecutionTiming::Before,
                ExecutionMode::CallOriginal);
        }
        else
        {
            RemoveHook(SilentAimHandle);
            SilentAimHandle = 0;
        }

        // Toggle projectile silent aim alongside — it uses a different hook
        // (Server_Fire on ProjectileLauncherBaseComponent) that catches
        // grenade/launcher projectiles, the one weapon family our PE hook
        // for SDK::UWeaponFireComponent::Fire never reaches.
        if (SilentAimEnabled) EnableProjectileSilentAim();
        else                  DisableProjectileSilentAim();

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
                static constexpr float kFOV     = 30.f;
                const FVector  CamLoc = CamMgr->CameraCachePrivate.POV.Location;
                const FRotator CamRot = CamMgr->CameraCachePrivate.POV.Rotation;
                const float PR = CamRot.Pitch * kDeg2Rad;
                const float YR = CamRot.Yaw   * kDeg2Rad;
                const FVector Forward{
                    std::cos(PR) * std::cos(YR),
                    std::cos(PR) * std::sin(YR),
                    std::sin(PR)
                };

                auto target = FindAimbotTargetPos(Player, CamMgr, CamLoc, Forward, kFOV);
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