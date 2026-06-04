// Aimbot_Projectile_ServerFire.cpp — UProjectileLauncherBaseComponent::Server_Fire.
//
// Server_Fire is a server RPC and goes through ProcessEvent dispatch, so our
// PE hook intercepts it. We rewrite the rotation quaternion in the FTransform
// parameter to point toward the target. In practice native code often re-reads
// the weapon mesh's barrel socket for the actual launch direction, so this
// write may not affect trajectory. See Aimbot_Projectile_Activate.cpp for the
// hook that IS the authoritative direction source for most launchers.
//
// NOTE: Projectile launcher redirection currently only works correctly for the
// Flaregun C++ class. Other launchers fail to redirect — the root cause is
// unknown but likely involves the muzzle mesh location. Deferred investigation.

#include "Aim_Internal.h"

#include <cmath>

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
        CallbackHandle ProjectileSilentAimHandle = 0;
    }

    void EnableProjectileSilentAim()
    {
        if (ProjectileSilentAimHandle) return;
        ProjectileSilentAimHandle = OnProcessEvent()
            .Name("Server_Fire").Class(UProjectileLauncherBaseComponent::StaticClass())
            .Bind([](UObject* Obj, UFunction*, void* Parms) {
                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/projectile] Server_Fire class={} obj={:p} outer={}",
                        Obj && Obj->Class ? Obj->Class->GetName() : "?",
                        static_cast<void*>(Obj),
                        Obj && Obj->Outer && Obj->Outer->Class ? Obj->Outer->Class->GetName() : "?");
                if (!Parms) return;

                auto* Launcher = Cast<UProjectileLauncherBaseComponent>(Obj);
                if (!IsValidOf<UProjectileLauncherBaseComponent>(Launcher))
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/projectile] reject: not UProjectileLauncherBaseComponent");
                    return;
                }

                APlayerCharacter* Player = GetLocalPlayer();
                if (!IsValidOf<APlayerCharacter>(Player))
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/projectile] reject: no local player");
                    return;
                }

                auto* OwnerWeapon = Cast<AAmmoDrivenWeapon>(Obj->Outer);
                if (!OwnerWeapon || OwnerWeapon->Character != Player || !OwnerWeapon->IsEquipped)
                {
                    if constexpr (Debug::LogSilentAim)
                        info("[silentaim/projectile] reject: outer not equipped weapon of local (outer={:p} char={:p} equipped={})",
                            static_cast<void*>(OwnerWeapon),
                            static_cast<void*>(OwnerWeapon ? OwnerWeapon->Character : nullptr),
                            OwnerWeapon ? OwnerWeapon->IsEquipped : false);
                    return;
                }

                AFSDPlayerController* Ctrl = Cast<AFSDPlayerController>(Player->Controller);
                if (!IsValidOf<AFSDPlayerController>(Ctrl)) return;
                APlayerCameraManager* CamMgr = Ctrl->PlayerCameraManager;
                if (!CamMgr) return;
                if (IsOnSpaceRig())
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/projectile] reject: on space rig");
                    return;
                }

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

                AItem* equipped = Cast<AItem>(OwnerWeapon);
                auto target = FindAimbotTargetPos(Player, CamMgr, CamLoc, Forward,
                    Config::ResolveSilentAimFOV(equipped),
                    Config::ResolveSilentAimRequireLOS(equipped));
                if (!target)
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/projectile] no target found");
                    return;
                }
                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/projectile] target pos=({:.0f},{:.0f},{:.0f})", target->X, target->Y, target->Z);

                // Params layout: Server_Fire(FTransform Transform, FVector_NetQuantizeNormal
                //                            initialBonusVelocity, AProjectileBase* DormentProjectile,
                //                            bool notifyClients).
                // FTransform: rotation FQuat at +0x00, translation FVector at +0x10, scale FVector at +0x20.
                // We rebuild the rotation quaternion from a desired forward direction.
                auto* parms = static_cast<uint8*>(Parms);
                FVector* trans = reinterpret_cast<FVector*>(parms + 0x10);
                FVector  Dir   = *target - *trans;
                const float Dist = std::sqrt(Dir.X*Dir.X + Dir.Y*Dir.Y + Dir.Z*Dir.Z);
                if (Dist < 1.f) return;
                Dir.X /= Dist; Dir.Y /= Dist; Dir.Z /= Dist;

                // Quaternion from forward vector (forward = +X in UE convention).
                // axis = X cross Dir; sin/cos derived from dot.
                const float dotF = Dir.X;
                FVector axis{ 0.f, -Dir.Z, Dir.Y };
                const float aLen = std::sqrt(axis.X*axis.X + axis.Y*axis.Y + axis.Z*axis.Z);
                struct FQuat { float X, Y, Z, W; } q{ 0, 0, 0, 1 };
                if (aLen > 1e-6f)
                {
                    const float half = 0.5f * std::acos(std::clamp(dotF, -1.f, 1.f));
                    const float sH   = std::sin(half) / aLen;
                    q.X = axis.X * sH;
                    q.Y = axis.Y * sH;
                    q.Z = axis.Z * sH;
                    q.W = std::cos(half);
                }
                *reinterpret_cast<FQuat*>(parms) = q;
            });
    }

    void DisableProjectileSilentAim()
    {
        if (!ProjectileSilentAimHandle) return;
        RemoveHook(ProjectileSilentAimHandle);
        ProjectileSilentAimHandle = 0;
    }

} // namespace AimAssist
