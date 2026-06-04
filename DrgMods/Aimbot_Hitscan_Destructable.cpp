// Aimbot_Hitscan_Destructable.cpp — UHitscanComponent::Server_RegisterHit_Destructable.
//
// Used by lock-on rifle and other weapons that target destructible armor parts.
// Same logic as HitOptimize (Hitscan_Standard), but the RPC also carries
// BoneIndex — the server uses it for bone-aware damage routing (armor section
// multipliers, breakable armor accounting). We update BoneIndex to match the
// chosen body so server-side damage routing lands on the right part.
// BoneIndex in the RPC is uint8; UE skeletons rarely exceed 255 bones on
// enemies, but we guard the truncation anyway.

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
        CallbackHandle HitscanDestructableOptimizeHandle = 0;
    }

    void EnableHitscanDestructableOptimize()
    {
        if (HitscanDestructableOptimizeHandle) return;
        HitscanDestructableOptimizeHandle = OnProcessEvent()
            .Name("Server_RegisterHit_Destructable").Class(UHitscanComponent::StaticClass())
            .Bind([](UObject* /*Obj*/, UFunction*, void* Parms) {
                if (!Parms) return;
                auto* p = reinterpret_cast<Params::HitscanComponent_Server_RegisterHit_Destructable*>(Parms);
                if (!p->Target)
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/destructable] reject: null Target");
                    return;
                }
                auto* Enemy = Cast<AFSDPawn>(p->Target->GetOwner());
                if (!Enemy)
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/destructable] reject: owner not AFSDPawn");
                    return;
                }

                const float baseline = (p->PhysMaterial)
                    ? p->PhysMaterial->DamageMultiplier
                    : 1.f;

                auto best = FindBestDamageBody(Enemy, baseline);
                if (!best)
                {
                    if constexpr (Debug::LogSilentAim)
                        info("[silentaim/destructable] no improvement (baseline={:.2f}) — pass-through", baseline);
                    return;
                }

                p->Location.X   = best->pos.X;
                p->Location.Y   = best->pos.Y;
                p->Location.Z   = best->pos.Z;
                p->PhysMaterial = best->physMat;

                // Update BoneIndex so server routes damage to the right skeletal piece.
                if (best->mesh && !best->boneName.IsNone())
                {
                    const int32 idx = best->mesh->GetBoneIndex(best->boneName);
                    if (idx >= 0 && idx <= 255)
                        p->BoneIndex = static_cast<uint8>(idx);
                }

                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/destructable] enemy={} baseline={:.2f} → best={:.2f} boneIdx={}",
                        Enemy->Class ? Enemy->Class->GetName() : "?",
                        baseline, best->multiplier, p->BoneIndex);
            });
    }

    void DisableHitscanDestructableOptimize()
    {
        if (!HitscanDestructableOptimizeHandle) return;
        RemoveHook(HitscanDestructableOptimizeHandle);
        HitscanDestructableOptimizeHandle = 0;
    }

} // namespace AimAssist
