#pragma once
// Aim.h — Aimbot + RCS + Silent Aim system.
//
// Self-contained module that owns:
//   • Per-shot recoil compensation (RCS) on pitch + yaw, run in a single
//     camera-frame-gated loop;
//   • Aimbot targeting (driven from inside the same loop, so it composes
//     with RCS instead of fighting it);
//   • Silent aim — redirects the bullet direction at fire-time without
//     touching the camera, via a UWeaponFireComponent::Fire hook;
//   • Weakpoint-targeting helpers + the `wpinfo` debug command.
//
// Wire-up from Commands.cpp:
//   AimAssist::RegisterCommands(handler);
//   AimAssist::RegisterKeybinds();

#include <vector>
#include "Lib_Forward.h"   // FName

class CommandHandler;

namespace AimAssist
{
    // ── Tunable target filters ────────────────────────────────────────────────
    //
    // ForceIncludeClasses: exact UClass::Name match → always considered.
    // IgnoreBaseClasses:   IsChildOfByName subclass match → skipped, unless
    //                      the enemy is elite or boss (those override ignore).

    extern std::vector<SDK::FName> IgnoreBaseClasses;
    extern std::vector<SDK::FName> ForceIncludeClasses;

    // ── Runtime state (read-only externally) ─────────────────────────────────

    extern bool   RecoilEnabled;
    extern float  RCSFactor;        // [0, 2]; 0=off, 1=full, >1=over-correct
    extern bool   AimbotKeyHeld;
    extern bool   SilentAimEnabled;

    // ── Keybind / toggle handlers ────────────────────────────────────────────

    void AimbotPressed();
    void AimbotReleased();
    void ToggleRecoilControl();
    void ToggleSilentAim();

    // ── Setup ─────────────────────────────────────────────────────────────────

    void RegisterCommands(CommandHandler& handler);
    void RegisterKeybinds();
}
