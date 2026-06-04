// Aimbot_DebugConfig.cpp — diagnostic tools: wpinfo command and firespy toggle.
//
// Compile-time debug flags (Debug::LogSilentAim, Debug::ShowGetComponentByClassQuery)
// are inline constexpr values in Aim_Internal.h::Debug — edit there and rebuild.

#include "Aim_Internal.h"

#include "SDK/SDK/CoreUObject_classes.hpp"
#include "SDK/SDK/Engine_classes.hpp"
#include "SDK/SDK/FSD_classes.hpp"

using namespace SDK;
using namespace ObjectCast;
using namespace GameHooks;

namespace AimAssist
{
    namespace
    {
        CallbackHandle FireSpyHandle  = 0;
        bool           FireSpyEnabled = false;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  wpinfo — dumps weakpoint physics bodies for the nearest enemy.
    // ─────────────────────────────────────────────────────────────────────────

    void WPInfo(const CommandContext& /*ctx*/)
    {
        if (IsOnSpaceRig()) { info("[wpinfo] on space rig"); return; }

        std::vector<AFSDPawn*> Enemies = GetAliveNonFriendlies();
        if (Enemies.empty()) { info("[wpinfo] no enemies"); return; }

        APlayerCharacter* LocalPlayer = GetLocalPlayer();
        FVector CamLoc = {};
        if (IsValidOf<APlayerCharacter>(LocalPlayer))
            if (auto* Ctrl = Cast<AFSDPlayerController>(LocalPlayer->Controller))
                if (auto* CamMgr = Ctrl->PlayerCameraManager)
                    CamLoc = CamMgr->CameraCachePrivate.POV.Location;

        AFSDPawn* Target = nullptr;
        float BestDist = FLT_MAX;
        for (AFSDPawn* E : Enemies)
        {
            if (!IsValidOf<AFSDPawn>(E)) continue;
            FVector D = E->K2_GetActorLocation() - CamLoc;
            float Dist = D.X * D.X + D.Y * D.Y + D.Z * D.Z;
            if (Dist < BestDist) { BestDist = Dist; Target = E; }
        }
        if (!Target) { info("[wpinfo] no valid target"); return; }

        const char* Branch = Cast<AEnemyDeepPathfinderCharacter>(Target) ? "DeepPathfinder"
            : Cast<AEnemyPawn>(Target) ? "EnemyPawn"
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

            for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); ++i)
            {
                auto* Body = PhysAsset->SkeletalBodySetups[i];
                if (!Body) continue;
                auto* FSDMat = Body->PhysMaterial ? Cast<UFSDPhysicalMaterial>(Body->PhysMaterial) : nullptr;
                if (!FSDMat || !FSDMat->IsWeakPoint) continue;

                const int32   BoneIdx = Mesh->GetBoneIndex(Body->BoneName);
                const FVector Pos = Mesh->GetSocketLocation(Body->BoneName);
                const bool    bVis = IsValidOf<APlayerCharacter>(LocalPlayer)
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

    // ─────────────────────────────────────────────────────────────────────────
    //  firespy — logs every UFunction dispatched via ProcessEvent on the local
    //  player's equipped weapon (and its components) until toggled off.
    //  Use to discover which UFunctions fire during a shot for hitscan diagnosis.
    // ─────────────────────────────────────────────────────────────────────────

    void ToggleFireSpy()
    {
        FireSpyEnabled = !FireSpyEnabled;
        if (FireSpyEnabled)
        {
            FireSpyHandle = OnProcessEventAll(
                [](UObject* Obj, UFunction* Fn, void* Parms) {
                    if (!Obj || !Fn) return;
                    APlayerCharacter* Player = GetLocalPlayer();
                    if (!IsValidOf<APlayerCharacter>(Player)) return;

                    static const FName ReceiveTick      (L"ReceiveTick");
                    static const FName GetComponentByClass(L"GetComponentByClass");
                    static const FName Flicker          (L"Flicker Brightness__UpdateFunc");
                    static const FName HeatUpdated      (L"HeatUpdated");

                    if (AnyOf<FName>(Fn->Name, ReceiveTick, Flicker, HeatUpdated)) return;

                    AItem* Equipped = Player->InventoryComponent
                        ? Player->InventoryComponent->GetEquippedItem()
                        : nullptr;
                    if (!Equipped) return;

                    bool match = false;
                    if (Obj == Equipped) match = true;
                    else if (auto* outerActor = Cast<AItem>(Obj->Outer);
                             outerActor && outerActor == Equipped) match = true;

                    if (!match) return;

                    // GetComponentByClass is high-volume; gated by a debug flag.
                    if (Fn->Name == GetComponentByClass)
                    {
                        if constexpr (Debug::ShowGetComponentByClassQuery)
                        {
                            UClass* queried = Parms
                                ? *reinterpret_cast<UClass**>(Parms)
                                : nullptr;
                            info("[firespy] {}::GetComponentByClass(class={}) (Obj={:p})",
                                Obj->Class ? Obj->Class->GetName() : "?",
                                queried ? queried->GetName() : "<null>",
                                static_cast<void*>(Obj));
                        }
                        return;
                    }

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

} // namespace AimAssist
