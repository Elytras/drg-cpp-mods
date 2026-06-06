// Aimbot_Base.cpp — public state, RCS+aimbot loop, ToggleSilentAim, command/keybind registration.
//
// Entry point for the AimAssist module. Owns the engine-tick RCS loop and
// orchestrates the Enable/Disable calls for all silent-aim subsystems.

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
    // ─────────────────────────────────────────────────────────────────────────
    //  Public state (declared extern in Aim.h)
    // ─────────────────────────────────────────────────────────────────────────

    std::vector<FName> IgnoreBaseClasses{};
    std::vector<FName> ForceIncludeClasses{};

    bool  RecoilEnabled   = false;
    float RCSFactor       = 1.0f;
    bool  AimbotKeyHeld   = false;
    bool  SilentAimEnabled = false;

    // ─────────────────────────────────────────────────────────────────────────
    //  Internal state
    // ─────────────────────────────────────────────────────────────────────────

    HitscanRedirect g_PendingRedirect{};

    namespace
    {
        bool  RCSInitialized   = false;
        float RCSDesiredPitch  = 0.f;
        float RCSPrevCtrlPitch = 0.f;
        float RCSDesiredYaw    = 0.f;
        float RCSPrevCtrlYaw   = 0.f;

        CallbackHandle RCSHandle = 0;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Keybind flags
    // ─────────────────────────────────────────────────────────────────────────

    void AimbotPressed()  { AimbotKeyHeld = true; }
    void AimbotReleased() { AimbotKeyHeld = false; }

    // ─────────────────────────────────────────────────────────────────────────
    //  Unified RCS + aimbot loop
    // ─────────────────────────────────────────────────────────────────────────

    void ToggleRecoilControl()
    {
        RecoilEnabled  = !RecoilEnabled;
        RCSInitialized = false;

        if (RecoilEnabled)
        {
            auto normP = [](float a) -> float {
                a = std::fmod(a, 360.f);
                if (a > 180.f)  a -= 360.f;
                if (a < -180.f) a += 360.f;
                return a;
            };

            // Driven from UEngine::Tick (post-original): state from this frame,
            // written rotation picked up by the next frame's input pass.
            RCSHandle = OnEngineTick(
                [normP](UEngine* /*Engine*/, float /*DeltaSeconds*/, bool /*bIdleMode*/)
                {
                    if (!RecoilEnabled) return;

                    APlayerCharacter* Player = GetLocalPlayer();
                    if (!IsValidOf<APlayerCharacter>(Player)) return;

                    if (!Player->IsAlive()) return;
                    static ECharacterState State = ECharacterState::Invalid;

                    static auto IsValidState = [](ECharacterState s) {
                        return s == ECharacterState::Walking ||
                               s == ECharacterState::ZipLine ||
                               s == ECharacterState::Falling;
                    };

                    if (!IsValidState(State))
                    {
                        // Delay RCS for one tick after respawn to avoid spinning.
                        State = Player->GetCurrentState();
                        return;
                    }

                    State = Player->GetCurrentState();
                    if (!IsValidState(State)) return;

                    AFSDPlayerController* Ctrl = Cast<AFSDPlayerController>(Player->Controller);
                    if (!IsValidOf<AFSDPlayerController>(Ctrl)) return;

                    APlayerCameraManager* CamMgr = Ctrl->PlayerCameraManager;
                    if (!CamMgr) return;

                    const FRotator ctrlRot   = Ctrl->GetControlRotation();
                    const FRotator camRot    = CamMgr->CameraCachePrivate.POV.Rotation;
                    const float    ctrlPitch = normP(ctrlRot.Pitch);
                    const float    camPitch  = normP(camRot.Pitch);
                    const float    ctrlYaw   = normP(ctrlRot.Yaw);
                    const float    camYaw    = normP(camRot.Yaw);

                    if (!RCSInitialized)
                    {
                        RCSDesiredPitch  = ctrlPitch;
                        RCSPrevCtrlPitch = ctrlPitch;
                        RCSDesiredYaw    = ctrlYaw;
                        RCSPrevCtrlYaw   = ctrlYaw;
                        RCSInitialized   = true;
                        return;
                    }

                    // ── Gimbal-flip protection ─────────────────────────────
                    // When cam pitch crosses ±90°, UE reflects pitch and flips
                    // yaw by 180°. Both the cam↔ctrl yaw offset and the per-
                    // frame ctrl yaw delta spike to ~180° on that boundary.
                    // Real recoil/input never produces a single-frame 90°+ yaw,
                    // so use that as the flip signal — resync and skip the frame.
                    const float ctrlPitchDelta = normP(ctrlPitch - RCSPrevCtrlPitch);
                    const float ctrlYawDelta   = normP(ctrlYaw   - RCSPrevCtrlYaw);
                    const float pitchOffset    = camPitch - ctrlPitch;
                    const float yawOffset      = normP(camYaw - ctrlYaw);
                    const float gimbalThresh = Config::GetGlobals()->GimbalFlipThresholdDeg;
                    if (std::abs(yawOffset)    > gimbalThresh ||
                        std::abs(ctrlYawDelta) > gimbalThresh ||
                        std::abs(pitchOffset)  > gimbalThresh)
                    {
                        RCSDesiredPitch  = ctrlPitch;
                        RCSDesiredYaw    = ctrlYaw;
                        RCSPrevCtrlPitch = ctrlPitch;
                        RCSPrevCtrlYaw   = ctrlYaw;
                        return;
                    }

                    // ── Aimbot integration ─────────────────────────────────
                    // Silent aim takes precedence: when it's on, bullets are
                    // redirected at fire time and the camera should stay put.
                    bool aimbotSet = false;
                    if (AimbotKeyHeld && !SilentAimEnabled)
                    {
                        static constexpr float kDeg2Rad = 3.14159265f / 180.f;

                        const FVector CamLoc = CamMgr->CameraCachePrivate.POV.Location;
                        const float   PR = camRot.Pitch * kDeg2Rad;
                        const float   YR = camRot.Yaw * kDeg2Rad;
                        const FVector Forward{
                            std::cos(PR) * std::cos(YR),
                            std::cos(PR) * std::sin(YR),
                            std::sin(PR)
                        };
                        AItem* equipped = Player->InventoryComponent
                            ? Player->InventoryComponent->GetEquippedItem() : nullptr;
                        const float fov = Config::ResolveAimbotFOV(equipped);
                        if (auto target = FindAimbotTargetPos(Player, CamMgr, CamLoc, Forward, fov))
                        {
                            const FRotator AimRot = UKismetMathLibrary::MakeRotFromX(*target - CamLoc);
                            RCSDesiredPitch = std::clamp(normP(AimRot.Pitch), -90.f, 90.f);
                            RCSDesiredYaw   = normP(AimRot.Yaw);
                            aimbotSet = true;
                        }
                    }

                    if (!aimbotSet)
                    {
                        RCSDesiredPitch += ctrlPitchDelta;
                        RCSDesiredPitch = std::clamp(RCSDesiredPitch, -90.f, 90.f);
                        RCSDesiredYaw  += ctrlYawDelta;
                        RCSDesiredYaw   = normP(RCSDesiredYaw);
                    }

                    // ── Recoil compensation (always) ───────────────────────
                    // ctrl = desired - recoil_offset  →  camera = ctrl + offset = desired.
                    const float newPitch = std::clamp(
                        RCSDesiredPitch - pitchOffset * RCSFactor, -90.f, 90.f);
                    const float newYaw   = normP(
                        RCSDesiredYaw   - yawOffset   * RCSFactor);

                    FRotator rot = ctrlRot;
                    rot.Pitch = newPitch;
                    rot.Yaw   = newYaw;
                    Ctrl->SetControlRotation(rot);
                    RCSPrevCtrlPitch = newPitch;
                    RCSPrevCtrlYaw   = newYaw;
                },
                ExecutionTiming::After);
        }
        else if (RCSHandle != 0)
        {
            EngineTickHook::Get().RemoveCallback(RCSHandle);
            RCSHandle = 0;
        }
        info("[recoil] {}", RecoilEnabled ? "ON" : "OFF");
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  ToggleSilentAim — enables/disables all silent-aim subsystems together.
    //  Each Enable/Disable pair is defined in its corresponding Aimbot_*.cpp.
    // ─────────────────────────────────────────────────────────────────────────

    void ToggleSilentAim()
    {
        SilentAimEnabled = !SilentAimEnabled;

        if (SilentAimEnabled)
        {
            EnableHitscanSilentAim();
            EnableHitscanRpcRedirect();
            EnableHitscanHitOptimize();
            EnableHitscanMultiOptimize();
            EnableHitscanDestructableOptimize();
            EnableHitscanCapsuleOptimize();
            EnableHitscanReflectionOptimize();
            EnableProjectileSilentAim();
            EnableProjectileActivate();
        }
        else
        {
            DisableHitscanSilentAim();
            DisableHitscanRpcRedirect();
            DisableHitscanHitOptimize();
            DisableHitscanMultiOptimize();
            DisableHitscanDestructableOptimize();
            DisableHitscanCapsuleOptimize();
            DisableHitscanReflectionOptimize();
            DisableProjectileSilentAim();
            DisableProjectileActivate();
        }

        info("[silent aim] {}", SilentAimEnabled ? "ON" : "OFF");
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Command + keybind registration
    // ─────────────────────────────────────────────────────────────────────────

    void RegisterCommands(CommandHandler& handler)
    {
        handler.Register("norecoil",
            [](const CommandContext&) { ToggleRecoilControl(); },
            "Player",
            R"(Toggle unified RCS+aimbot loop. Aimbot fires on MouseLeft Held.)");

        handler.Register("rcsfactor",
            [](const CommandContext& ctx) {
                if (ctx.ArgCount() < 2) {
                    info("[rcsfactor] current = {:.2f}", RCSFactor);
                    return;
                }
                const float v = SafeStof(ctx.Arg(1));
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
            R"(Toggle: log every UFunction dispatched via PE on the equipped weapon. Use to find what fires during a shot.)");

        handler.Register("reloadaimcfg",
            [](const CommandContext&) { Config::ReloadGlobals(); },
            "Player",
            R"(Reload aimbot.yaml from disk (FOV, sampling, class lists, weapon overrides). Keybind changes need a full DLL reload.)");
    }

    void RegisterKeybinds()
    {
        using enum Trigger;
        using enum Focus;

        auto kb = Config::GetGlobals();
        BindingOptions aimDown{ Press,   Game, false }; aimDown.label = "Aim assist (hold)";
        BindingOptions aimUp  { Release, Game, false }; aimUp.label   = "Aim assist (release)";
        KeyBindings::RegisterGameThread(kb->AimbotKey, kb->AimbotMod, AimbotPressed,  aimDown);
        KeyBindings::RegisterGameThread(kb->AimbotKey, kb->AimbotMod, AimbotReleased, aimUp);

        BindingOptions rcsOpts{ Press, Game, false }; rcsOpts.label = "Toggle recoil control";
        BindingOptions saOpts { Press, Game, false }; saOpts.label  = "Toggle silent aim";
        KeyBindings::RegisterGameThread(kb->RecoilToggleKey,   kb->RecoilToggleMod,   ToggleRecoilControl, rcsOpts);
        KeyBindings::RegisterGameThread(kb->SilentAimToggleKey, kb->SilentAimToggleMod, ToggleSilentAim,    saOpts);
    }

} // namespace AimAssist
