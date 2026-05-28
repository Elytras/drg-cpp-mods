// Aimbot_Hitscan_Standard.cpp — hitscan hooks for UHitscanComponent / AAmmoDrivenWeapon.
//
// Three hooks, registered together in ToggleSilentAim:
//
//   (a) OnWeaponFired (AAmmoDrivenWeapon) Before/CallOriginal
//       Sets Location in the PE-frame struct to the chosen WP position.
//       Native WeaponFireComponent::Fire re-reads it when dispatching
//       Server_RegisterHit_* RPCs. Also stashes target mesh + phys mat
//       into g_PendingRedirect for the terrain-redirect hook to consume.
//
//   (b) Server_RegisterHit_Terrain (UHitscanComponent) Before/SkipOriginal
//       When the native trace missed and queued a terrain RPC, replaces it
//       with a Server_RegisterHit call to the stashed enemy mesh + phys mat.
//       Pass-through when no redirect is pending (no enemy in FOV, etc.).
//
//   (c) Server_RegisterHit (UHitscanComponent) Before/CallOriginal
//       When the native trace hit an enemy, rewrites Location + PhysMaterial
//       to the highest-multiplier body on that enemy. Needed because native
//       code sets PhysMaterial independently from the trace's impact mat —
//       without this, damage is computed from the lower-tier mat even when
//       Location already points at the WP.

#include "Aim_Internal.h"

#include <cmath>

#include "SDK/SDK/CoreUObject_classes.hpp"
#include "SDK/SDK/Engine_classes.hpp"
#include "SDK/SDK/FSD_classes.hpp"
#include "SDK/SDK/FSD_parameters.hpp"

using namespace SDK;
using namespace ObjectCast;
using namespace GameHooks;

namespace AimAssist
{
    namespace
    {
        CallbackHandle HitscanSilentAimHandle    = 0;
        CallbackHandle HitscanRpcRedirectHandle  = 0;
        CallbackHandle HitscanHitOptimizeHandle  = 0;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  (a) OnWeaponFired — entry point for hitscan silent aim
    // ─────────────────────────────────────────────────────────────────────────

    void EnableHitscanSilentAim()
    {
        if (HitscanSilentAimHandle) return;
        HitscanSilentAimHandle = OnProcessEventByNameAndClass(
            "OnWeaponFired", AAmmoDrivenWeapon::StaticClass(),
            [](UObject* Obj, UFunction*, void* Parms) {
                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/hitscan] OnWeaponFired class={} obj={:p}",
                        Obj && Obj->Class ? Obj->Class->GetName() : "?", static_cast<void*>(Obj));
                if (!Parms) return;

                auto* Weapon = Cast<AAmmoDrivenWeapon>(Obj);
                if (!IsValidOf<AAmmoDrivenWeapon>(Weapon))
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/hitscan] reject: not AAmmoDrivenWeapon");
                    return;
                }

                APlayerCharacter* Player = GetLocalPlayer();
                if (!IsValidOf<APlayerCharacter>(Player))
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/hitscan] reject: no local player");
                    return;
                }
                if (Weapon->Character != Player || !Weapon->IsEquipped)
                {
                    if constexpr (Debug::LogSilentAim)
                        info("[silentaim/hitscan] reject: not equipped by local (char={:p} equipped={})",
                            static_cast<void*>(Weapon->Character), Weapon->IsEquipped);
                    return;
                }

                AFSDPlayerController* Ctrl = Cast<AFSDPlayerController>(Player->Controller);
                if (!IsValidOf<AFSDPlayerController>(Ctrl)) return;
                APlayerCameraManager* CamMgr = Ctrl->PlayerCameraManager;
                if (!CamMgr) return;
                if (IsOnSpaceRig())
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/hitscan] reject: on space rig");
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

                // Clear stale redirect state at shot start — covers the case
                // where a previous shot's _Terrain RPC never fired (sky shot, etc.).
                g_PendingRedirect = {};

                AItem* equipped = Cast<AItem>(Weapon);
                auto target = FindAimbotTarget(Player, CamMgr, CamLoc, Forward,
                    Config::ResolveSilentAimFOV(equipped),
                    Config::ResolveSilentAimRequireLOS(equipped));
                if (!target)
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/hitscan] no target found");
                    return;
                }

                auto* p = reinterpret_cast<Params::AmmoDrivenWeapon_OnWeaponFired*>(Parms);
                p->Location = target->pos;

                g_PendingRedirect.active  = true;
                g_PendingRedirect.target  = target->mesh;
                g_PendingRedirect.physMat = target->physMat;
                g_PendingRedirect.pos     = target->pos;

                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/hitscan] redirected to enemy={:p} pos=({:.0f},{:.0f},{:.0f}) mesh={:p} mat={:p}",
                        static_cast<void*>(target->enemy),
                        target->pos.X, target->pos.Y, target->pos.Z,
                        static_cast<void*>(target->mesh),
                        static_cast<void*>(target->physMat));
            },
            ClassMatchMode::ExactOrSubclass,
            ExecutionTiming::Before,
            ExecutionMode::CallOriginal);
    }

    void DisableHitscanSilentAim()
    {
        if (!HitscanSilentAimHandle) return;
        RemoveHook(HitscanSilentAimHandle);
        HitscanSilentAimHandle = 0;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  (b) Server_RegisterHit_Terrain — redirect terrain RPC to enemy variant
    // ─────────────────────────────────────────────────────────────────────────

    void EnableHitscanRpcRedirect()
    {
        if (HitscanRpcRedirectHandle) return;
        HitscanRpcRedirectHandle = OnProcessEventByNameAndClass(
            "Server_RegisterHit_Terrain", UHitscanComponent::StaticClass(),
            [](UObject* Obj, UFunction*, void* Parms) {
                auto* comp = Cast<UHitscanComponent>(Obj);
                auto* tp   = Parms
                    ? reinterpret_cast<Params::HitscanComponent_Server_RegisterHit_Terrain*>(Parms)
                    : nullptr;

                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/_terrain] entry comp={:p} parms={:p} pending.active={} pending.target={:p}",
                        static_cast<void*>(comp), Parms,
                        g_PendingRedirect.active, static_cast<void*>(g_PendingRedirect.target));

                bool redirected = false;
                if (comp && tp && g_PendingRedirect.active && g_PendingRedirect.target)
                {
                    comp->Server_RegisterHit(
                        tp->Location, tp->Normal,
                        g_PendingRedirect.target,
                        g_PendingRedirect.physMat);
                    redirected = true;
                    if constexpr (Debug::LogSilentAim) info("[silentaim/_terrain] redirected → Server_RegisterHit");
                }

                // Pass-through when no redirect — covers "no enemy in FOV",
                // null target, cast/param failure. The PE re-entrancy guard
                // routes this straight to OriginalProcessEvent without
                // retriggering our callback.
                if (!redirected && comp && tp)
                {
                    comp->Server_RegisterHit_Terrain(tp->Location, tp->Normal, tp->PhysMaterial);
                    if constexpr (Debug::LogSilentAim) info("[silentaim/_terrain] pass-through");
                }

                g_PendingRedirect = {};
            },
            ClassMatchMode::ExactOrSubclass,
            ExecutionTiming::Before,
            ExecutionMode::SkipOriginal);
    }

    void DisableHitscanRpcRedirect()
    {
        if (!HitscanRpcRedirectHandle) return;
        RemoveHook(HitscanRpcRedirectHandle);
        HitscanRpcRedirectHandle = 0;
        g_PendingRedirect = {};
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  (c) Server_RegisterHit — upgrade hit-location to best damage body
    //
    //  When the native trace lands on an enemy, rewrite Location + PhysMaterial
    //  to the body part with the highest damage multiplier on that enemy.
    //  Target stays as-is so the server's authority check passes.
    // ─────────────────────────────────────────────────────────────────────────

    void EnableHitscanHitOptimize()
    {
        if (HitscanHitOptimizeHandle) return;
        HitscanHitOptimizeHandle = OnProcessEventByNameAndClass(
            "Server_RegisterHit", UHitscanComponent::StaticClass(),
            [](UObject* /*Obj*/, UFunction*, void* Parms) {
                if (!Parms) return;
                auto* p = reinterpret_cast<Params::HitscanComponent_Server_RegisterHit*>(Parms);
                if (!p->Target)
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/optimize] reject: null Target");
                    return;
                }

                auto* Enemy = Cast<AFSDPawn>(p->Target->GetOwner());
                if (!Enemy)
                {
                    if constexpr (Debug::LogSilentAim) info("[silentaim/optimize] reject: target owner is not AFSDPawn");
                    return;
                }

                const float baseline = (p->PhysMaterial)
                    ? p->PhysMaterial->DamageMultiplier
                    : 1.f;

                auto best = FindBestDamageBody(Enemy, baseline);
                if (!best)
                {
                    if constexpr (Debug::LogSilentAim)
                        info("[silentaim/optimize] no improvement (baseline={:.2f}) — pass-through", baseline);
                    return;
                }

                if constexpr (Debug::LogSilentAim)
                    info("[silentaim/optimize] enemy={} baseline={:.2f} → best={:.2f} mat={:p}",
                        Enemy->Class ? Enemy->Class->GetName() : "?",
                        baseline, best->multiplier, static_cast<void*>(best->physMat));

                p->Location.X   = best->pos.X;
                p->Location.Y   = best->pos.Y;
                p->Location.Z   = best->pos.Z;
                p->PhysMaterial = best->physMat;
            },
            ClassMatchMode::ExactOrSubclass,
            ExecutionTiming::Before,
            ExecutionMode::CallOriginal);
    }

    void DisableHitscanHitOptimize()
    {
        if (!HitscanHitOptimizeHandle) return;
        RemoveHook(HitscanHitOptimizeHandle);
        HitscanHitOptimizeHandle = 0;
    }

} // namespace AimAssist
