// Aimbot_Hitscan_Reflection.cpp — UReflectionHitscanComponent (Wavecooker / reflective laser).
//
// UReflectionHitscanComponent fires a beam that can bounce off terrain surfaces.
// Two RPCs carry result data (both use the same FReflectiveHitscanHit struct):
//   Server_RegisterHit(Hit)           — direct hit on target
//   Server_RegisterHit_Reflection(Hit)— hit after a bounce
//
// FReflectiveHitscanHit relevant fields:
//   Component (UPrimitiveComponent*)  — owner must be AFSDPawn for enemy hits
//   HitLocation (FVector_NetQuantize) — world position of the hit
//   PhysMat (UFSDPhysicalMaterial*)   — physical material hit
//
// Both hooks share OptimizeReflectiveHit:
//   1. If Component owner is AFSDPawn → upgrade to best damage body (baseline from current PhysMat).
//   2. If Component owner is NOT AFSDPawn (terrain) AND g_PendingRedirect is set
//      → redirect to pending target and then optimize.

#include "Aim_Internal.h"

#include "SDK/SDK/FSD_classes.hpp"
#include "SDK/SDK/FSD_parameters.hpp"

using namespace SDK;
using namespace ObjectCast;
using namespace GameHooks;

namespace AimAssist
{
    namespace
    {
        CallbackHandle HitscanReflectionOptimizeHandle = 0;
        CallbackHandle HitscanReflectionReflectHandle  = 0;

        void OptimizeReflectiveHit(FReflectiveHitscanHit& hit)
        {
            auto* Enemy = Cast<AFSDPawn>(hit.Component ? hit.Component->GetOwner() : nullptr);

            if (!Enemy && g_PendingRedirect.active && g_PendingRedirect.target)
            {
                auto* PendingEnemy = Cast<AFSDPawn>(g_PendingRedirect.target->GetOwner());
                if (PendingEnemy)
                {
                    hit.Component = g_PendingRedirect.target;
                    Enemy = PendingEnemy;
                }
            }
            if (!Enemy) return;

            const float baseline = hit.PhysMat ? hit.PhysMat->DamageMultiplier : 1.f;
            auto best = FindBestDamageBody(Enemy, baseline);
            if (!best) return;

            hit.HitLocation.X = best->pos.X;
            hit.HitLocation.Y = best->pos.Y;
            hit.HitLocation.Z = best->pos.Z;
            if (best->physMat)
                hit.PhysMat = best->physMat;
        }
    }

    void EnableHitscanReflectionOptimize()
    {
        if (HitscanReflectionOptimizeHandle) return;

        HitscanReflectionOptimizeHandle = OnProcessEventByNameAndClass(
            "Server_RegisterHit", UReflectionHitscanComponent::StaticClass(),
            [](UObject* /*Obj*/, UFunction*, void* Parms) {
                if (!Parms) return;
                auto* p = reinterpret_cast<Params::ReflectionHitscanComponent_Server_RegisterHit*>(Parms);
                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/reflect] Server_RegisterHit comp={:p}",
                        static_cast<void*>(p->Hit.Component));
                OptimizeReflectiveHit(p->Hit);
            },
            ClassMatchMode::ExactOrSubclass,
            ExecutionTiming::Before,
            ExecutionMode::CallOriginal);

        HitscanReflectionReflectHandle = OnProcessEventByNameAndClass(
            "Server_RegisterHit_Reflection", UReflectionHitscanComponent::StaticClass(),
            [](UObject* /*Obj*/, UFunction*, void* Parms) {
                if (!Parms) return;
                auto* p = reinterpret_cast<Params::ReflectionHitscanComponent_Server_RegisterHit_Reflection*>(Parms);
                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/reflect] Server_RegisterHit_Reflection comp={:p}",
                        static_cast<void*>(p->Hit.Component));
                OptimizeReflectiveHit(p->Hit);
            },
            ClassMatchMode::ExactOrSubclass,
            ExecutionTiming::Before,
            ExecutionMode::CallOriginal);
    }

    void DisableHitscanReflectionOptimize()
    {
        if (HitscanReflectionOptimizeHandle)
        {
            RemoveHook(HitscanReflectionOptimizeHandle);
            HitscanReflectionOptimizeHandle = 0;
        }
        if (HitscanReflectionReflectHandle)
        {
            RemoveHook(HitscanReflectionReflectHandle);
            HitscanReflectionReflectHandle = 0;
        }
    }

} // namespace AimAssist
