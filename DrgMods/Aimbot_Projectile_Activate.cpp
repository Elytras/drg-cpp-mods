// Aimbot_Projectile_Activate.cpp — AProjectileBase::Activate direction override.
//
// The launcher's Server_Fire RPC is just the network-replication trigger.
// The actual projectile direction is set in AProjectileBase::Activate(
// owningActor, Origin, Direction, initialBonusVelocity), called by the launcher
// after spawning the projectile. Origin comes from the weapon-mesh barrel socket
// — that is the native source of truth for launch direction, not Server_Fire's
// FTransform.
//
// Hooking Activate Before, we rewrite p->Direction to point from Origin toward
// the OnWeaponFired-stashed target position. Works for straight-flying
// projectiles (rockets, plasma, line cutter). Ballistic projectiles (grenades)
// still arc short — needs elevation-angle prediction for proper trajectory; deferred.
//
// NOTE: Only confirmed working for the Flaregun C++ class. See notes in
// Aimbot_Projectile_ServerFire.cpp for the broader redirection problem.

#include "Aim_Internal.h"

#include <cmath>

#include "SDK/SDK/FSD_classes.hpp"
#include "SDK/SDK/FSD_parameters.hpp"

using namespace SDK;
using namespace ObjectCast;
using namespace GameHooks;

namespace AimAssist
{
    namespace
    {
        CallbackHandle ProjectileActivateHandle = 0;
    }

    void EnableProjectileActivate()
    {
        if (ProjectileActivateHandle) return;
        ProjectileActivateHandle = OnProcessEventByNameAndClass(
            "Activate", AProjectileBase::StaticClass(),
            [](UObject* /*Obj*/, UFunction*, void* Parms) {
                if (!Parms) return;
                if (!g_PendingRedirect.active) return;

                auto* p = reinterpret_cast<Params::ProjectileBase_Activate*>(Parms);

                // Only redirect projectiles launched by the local player's
                // equipped weapon — keeps AI projectiles, friendlies, etc. out of scope.
                APlayerCharacter* Player = GetLocalPlayer();
                if (!IsValidOf<APlayerCharacter>(Player)) return;
                auto* OwnerWeapon = Cast<AAmmoDrivenWeapon>(p->owningActor);
                if (!OwnerWeapon || OwnerWeapon->Character != Player || !OwnerWeapon->IsEquipped)
                {
                    if constexpr (Debug::LogSilentAim)
                        info("[silentaim/activate] reject: owningActor not local-equipped weapon ({:p})",
                            static_cast<void*>(p->owningActor));
                    return;
                }

                FVector dir{
                    g_PendingRedirect.pos.X - p->Origin.X,
                    g_PendingRedirect.pos.Y - p->Origin.Y,
                    g_PendingRedirect.pos.Z - p->Origin.Z,
                };
                const float dist = std::sqrt(dir.X * dir.X + dir.Y * dir.Y + dir.Z * dir.Z);
                if (dist < 1.f) return;
                dir.X /= dist; dir.Y /= dist; dir.Z /= dist;

                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/activate] origin=({:.0f},{:.0f},{:.0f}) → target=({:.0f},{:.0f},{:.0f}) dir=({:.2f},{:.2f},{:.2f})",
                        p->Origin.X, p->Origin.Y, p->Origin.Z,
                        g_PendingRedirect.pos.X, g_PendingRedirect.pos.Y, g_PendingRedirect.pos.Z,
                        dir.X, dir.Y, dir.Z);

                p->Direction.X = dir.X;
                p->Direction.Y = dir.Y;
                p->Direction.Z = dir.Z;
            },
            ClassMatchMode::ExactOrSubclass,
            ExecutionTiming::Before,
            ExecutionMode::CallOriginal);
    }

    void DisableProjectileActivate()
    {
        if (!ProjectileActivateHandle) return;
        RemoveHook(ProjectileActivateHandle);
        ProjectileActivateHandle = 0;
    }

} // namespace AimAssist
