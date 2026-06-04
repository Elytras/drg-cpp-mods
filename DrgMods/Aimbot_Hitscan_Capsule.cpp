// Aimbot_Hitscan_Capsule.cpp — UCapsuleHitscanComponent (capsule-sweep weapons, e.g. Experimental arc-welder).
//
// UCapsuleHitscanComponent packs all hits in the same FMultiHitScanHits format as
// UMultiHitscanComponent: parallel Hits/Components/PhysicalMaterials arrays.
// Same 1-based index scheme: slot 0 is null sentinel, terrain pellets use
// compIdx = Components.Num() as an OOB sentinel.
// See Aimbot_Hitscan_Multi.cpp for full layout documentation.
// Terrain redirect: UEArrayAdd appends the target so old_Num becomes a valid index.

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
        CallbackHandle HitscanCapsuleOptimizeHandle = 0;
    }

    void EnableHitscanCapsuleOptimize()
    {
        if (HitscanCapsuleOptimizeHandle) return;
        HitscanCapsuleOptimizeHandle = OnProcessEvent()
            .Name("Server_RegisterHit").Class(UCapsuleHitscanComponent::StaticClass())
            .Bind([](UObject* /*Obj*/, UFunction*, void* Parms) {
                if (!Parms) return;
                auto* p = reinterpret_cast<Params::CapsuleHitscanComponent_Server_RegisterHit*>(Parms);
                auto& hits       = p->hitResults.Hits;
                auto& components = p->hitResults.Components;
                auto& materials  = p->hitResults.PhysicalMaterials;

                // Pre-loop: append redirect target so terrain pellets resolve.
                // Terrain pellets carry compIdx = old_components.Num() (OOB sentinel).
                // After UEArrayAdd the new last slot IS that sentinel index.
                if (g_PendingRedirect.active && g_PendingRedirect.target)
                {
                    if (Cast<AFSDPawn>(g_PendingRedirect.target->GetOwner()))
                    {
                        components.AddGrow(g_PendingRedirect.target);
                        materials.AddGrow(g_PendingRedirect.physMat);
                    }
                }

                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/capsule] entry hits={} comps={} mats={}",
                        hits.Num(), components.Num(), materials.Num());

                int32 modified = 0;
                for (int32 i = 0; i < hits.Num(); ++i)
                {
                    auto& hit = hits[i];

                    if (hit.TargetComponentIndex >= components.Num())
                    {
                        if constexpr (Debug::LogSilentAim)
                            info("[silentaim/capsule]   [{}] SKIP OOB compIdx={} comps={}", i,
                                hit.TargetComponentIndex, components.Num());
                        continue;
                    }

                    auto* comp  = components[hit.TargetComponentIndex];
                    auto* Enemy = Cast<AFSDPawn>(comp ? comp->GetOwner() : nullptr);

                    if (!Enemy && g_PendingRedirect.active && g_PendingRedirect.target)
                    {
                        auto* PendingEnemy = Cast<AFSDPawn>(g_PendingRedirect.target->GetOwner());
                        if (PendingEnemy)
                        {
                            components[hit.TargetComponentIndex] = g_PendingRedirect.target;
                            Enemy = PendingEnemy;
                        }
                    }
                    if (!Enemy) continue;

                    auto best = FindBestDamageBody(Enemy, 0.f);
                    if (!best) continue;

                    hit.HitLocation.X = best->pos.X;
                    hit.HitLocation.Y = best->pos.Y;
                    hit.HitLocation.Z = best->pos.Z;
                    if (best->physMat && hit.PhysicalMaterialIndex < materials.Num())
                        materials[hit.PhysicalMaterialIndex] = best->physMat;
                    ++modified;
                }

                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/capsule] done: modified={} total={}", modified, hits.Num());
            });
    }

    void DisableHitscanCapsuleOptimize()
    {
        if (!HitscanCapsuleOptimizeHandle) return;
        RemoveHook(HitscanCapsuleOptimizeHandle);
        HitscanCapsuleOptimizeHandle = 0;
    }

} // namespace AimAssist
