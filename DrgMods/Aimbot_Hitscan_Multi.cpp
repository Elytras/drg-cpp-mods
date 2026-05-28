// Aimbot_Hitscan_Multi.cpp — UMultiHitscanComponent (shotguns / multi-pellet weapons).
//
// UMultiHitscanComponent has only Server_RegisterHit (no _Terrain / _Destructable
// variants), so this single hook handles both redirection and optimization.
//
// FMultiHitScanHits layout:
//   Components[0]            — null sentinel; NEVER referenced by real pellets
//   Components[1..N-1]       — unique components hit (enemies, props, terrain)
//   Hits[i].TargetComponentIndex  → 1-based index into Components
//                                   terrain-miss sentinel = Components.Num() (OOB)
//   Hits[i].PhysicalMaterialIndex → 1-based index into PhysicalMaterials
//                                   sentinel = PhysicalMaterials.Num() (OOB)
//
// Redirect strategy:
//   Pre-loop: if a redirect is pending, UEArrayAdd the target/physMat onto
//   Components and PhysicalMaterials. The new index equals old_Num, which is
//   exactly the terrain-sentinel value carried by terrain-miss pellets, so those
//   pellets resolve naturally without index rewrites.
//
//   Per-pellet:
//   1. OOB sentinel (add failed / no redirect) → skip.
//   2. Valid non-pawn component → overwrite that slot with redirect target.
//   3. Resolved pawn → FindBestDamageBody, rewrite HitLocation + PhysMat.

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
        CallbackHandle HitscanMultiOptimizeHandle = 0;
    }

    void EnableHitscanMultiOptimize()
    {
        if (HitscanMultiOptimizeHandle) return;
        HitscanMultiOptimizeHandle = OnProcessEventByNameAndClass(
            "Server_RegisterHit", UMultiHitscanComponent::StaticClass(),
            [](UObject* /*Obj*/, UFunction*, void* Parms) {
                if (!Parms) return;
                auto* p = reinterpret_cast<Params::MultiHitscanComponent_Server_RegisterHit*>(Parms);
                auto& hits       = p->hitResults.Hits;
                auto& components = p->hitResults.Components;
                auto& materials  = p->hitResults.PhysicalMaterials;

                // ── Pre-loop: append redirect target so terrain pellets resolve ──
                // Terrain pellets carry compIdx = old_components.Num() (OOB sentinel).
                // After UEArrayAdd the new last slot IS that sentinel index, so terrain
                // pellets fall through the loop normally without any index rewriting.
                if (g_PendingRedirect.active && g_PendingRedirect.target)
                {
                    if (Cast<AFSDPawn>(g_PendingRedirect.target->GetOwner()))
                    {
                        components.AddGrow(g_PendingRedirect.target);
                        materials.AddGrow(g_PendingRedirect.physMat);
                    }
                }

                if constexpr (Debug::LogSilentAim)
                {
                    auto* pendOwner = g_PendingRedirect.target
                        ? g_PendingRedirect.target->GetOwner() : nullptr;
                    info("[silentaim/multi] entry hits={} comps={} mats={}"
                         "  redirect: active={} target={:p}({})",
                        hits.Num(), components.Num(), materials.Num(),
                        g_PendingRedirect.active,
                        static_cast<void*>(g_PendingRedirect.target),
                        pendOwner && pendOwner->Class ? pendOwner->Class->GetName() : "?");
                }

                int32 modified = 0;
                for (int32 i = 0; i < hits.Num(); ++i)
                {
                    auto& hit = hits[i];

                    if (hit.TargetComponentIndex >= components.Num())
                    {
                        if constexpr (Debug::LogSilentAim)
                            info("[silentaim/multi]   [{}] SKIP OOB compIdx={} comps={}", i,
                                hit.TargetComponentIndex, components.Num());
                        continue;
                    }

                    auto* comp  = components[hit.TargetComponentIndex];
                    auto* Enemy = Cast<AFSDPawn>(comp ? comp->GetOwner() : nullptr);

                    // Non-pawn valid component (e.g. prop/terrain slot 1+) → redirect.
                    if (!Enemy && g_PendingRedirect.active && g_PendingRedirect.target)
                    {
                        auto* PendingEnemy = Cast<AFSDPawn>(g_PendingRedirect.target->GetOwner());
                        if (PendingEnemy)
                        {
                            components[hit.TargetComponentIndex] = g_PendingRedirect.target;
                            Enemy = PendingEnemy;
                            if constexpr (Debug::LogSilentAim)
                                info("[silentaim/multi]   [{}] slot {} → redirected",
                                    i, hit.TargetComponentIndex);
                        }
                    }

                    if (!Enemy) continue;

                    // baseline=0: every pellet commits to the best body regardless of
                    // current mat. Fallback returns actor-center + null physMat when
                    // the enemy has no FSD phys mats.
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
                    info("[silentaim/multi] done: modified={} total={}", modified, hits.Num());
            },
            ClassMatchMode::ExactOrSubclass,
            ExecutionTiming::Before,
            ExecutionMode::CallOriginal);
    }

    void DisableHitscanMultiOptimize()
    {
        if (!HitscanMultiOptimizeHandle) return;
        RemoveHook(HitscanMultiOptimizeHandle);
        HitscanMultiOptimizeHandle = 0;
    }

} // namespace AimAssist
