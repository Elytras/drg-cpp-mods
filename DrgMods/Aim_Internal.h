#pragma once
// Aim_Internal.h — AimAssist module internal header.
// Included by all Aimbot_*.cpp files. Not part of the public API (see Aim.h).
//
// Contains:
//   • Config namespace: inline constexpr tunables + WeaponConfigOverride struct.
//     Heavy function bodies (EnsureOverridesLoaded, Resolve*) are in Aimbot_Config.cpp.
//   • Debug namespace: compile-time diagnostic flags (inline constexpr).
//     Edit these flags and rebuild to enable verbose logging for a subsystem.
//   • Shared internal types: HitscanRedirect, AimTargetInfo, BestDamageBody, BreakableWpState.
//   • Internal function declarations used across multiple Aimbot_*.cpp files.
//   • Enable/Disable declarations for ToggleSilentAim (Aimbot_Base.cpp) to call.

#include "Aim.h"
#include "Library.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "SDK/SDK/Basic.hpp"
#include "SDK/SDK/FSD_classes.hpp"

namespace AimAssist
{
    // ─────────────────────────────────────────────────────────────────────────
    //  Targeting — candidate metadata and selector registry.
    //  Built-in selectors + function bodies are in Aimbot_TargetSelection.cpp.
    // ─────────────────────────────────────────────────────────────────────────

    namespace Targeting
    {
        struct TargetCandidate
        {
            SDK::AFSDPawn* enemy = nullptr;
            float dot           = -2.f;     // dot(forward, normalize(enemy - cam)); higher = closer to crosshair
            float distSq        = FLT_MAX;  // |enemy - cam|^2
            bool  nearCrosshair = false;    // dot >= cos(FOV/2)
            bool  bodyVisible   = false;    // cam→actor LOS clear
            bool  hasWP         = false;    // phys asset declares any non-destroyed weakpoint
            bool  wpVisible     = false;    // ≥1 WP candidate has LOS
        };

        using SelectorFn = std::function<SDK::AFSDPawn*(const std::vector<TargetCandidate>&)>;

        void       Register(const std::string& name, SelectorFn fn);
        SelectorFn Get(const std::string& name);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Config — all tunables and per-weapon overrides.
    //  Compile-time constants (inline constexpr) live here so they're visible
    //  for if constexpr branches in every Aimbot_*.cpp TU.
    //  Heavy function bodies are in Aimbot_Config.cpp.
    // ─────────────────────────────────────────────────────────────────────────

    namespace Config
    {
        // ─────────────────────────────────────────────────────────────────────
        //  Global config — loaded from aimbot.yaml at first access.
        //  Searched in two locations (first match wins):
        //    1. Next to the DLL   (distribution: drop beside DrgMods.dll)
        //    2. ../../ from DLL   (dev: solution root, next to Drgmods.sln)
        //  Edit the file and call ReloadGlobals() (or 'reloadaimcfg' command)
        //  to hot-reload without restarting.  Note: keybind changes only take
        //  effect after a full DLL reload (they are registered once at startup).
        // ─────────────────────────────────────────────────────────────────────

        struct GlobalConfig
        {
            // ── FOV cones (degrees) ──────────────────────────────────────────
            float AimbotFOVDeg           = 0.f;     // 0 = disabled
            float SilentAimFOVDeg        = 360.f;   // 360 = always redirect

            // ── Weakpoint sampling ───────────────────────────────────────────
            float BodyRadiusScale        = 0.75f;
            float BodyRadiusFallback     = 15.f;
            // Tried in order; first visible candidate wins as the aim position.
            std::vector<SDK::FVector> WpSampleOffsets = {
                { 0., 0., 0.},
                { 1., 0., 0.}, {-1., 0., 0.},
                { 0., 1., 0.}, { 0.,-1., 0.},
                { 0., 0., 1.}, { 0., 0.,-1.},
            };

            // ── RCS gimbal-flip threshold (degrees) ──────────────────────────
            float GimbalFlipThresholdDeg = 90.f;

            // ── Silent aim ───────────────────────────────────────────────────
            bool  SilentAimRequireLOS   = false;

            // ── Keybinds (read once at RegisterKeybinds; reload needs restart)
            Key   AimbotKey             = Key::MouseLeft;
            Mod   AimbotMod             = Mod::None;
            Key   RecoilToggleKey       = Key::R;
            Mod   RecoilToggleMod       = Mod::Ctrl;
            Key   SilentAimToggleKey    = Key::F3;
            Mod   SilentAimToggleMod    = Mod::Ctrl;

            // ── Class filter lists ───────────────────────────────────────────
            // If ignored_item_classes is absent from the YAML, populated from
            // StaticClass() lookups at load time (C++ fallback).
            std::vector<SDK::FName> IgnoredItemClasses;
            // If default_ignore_base_classes is absent, seeded from C++ fallback.
            std::vector<SDK::FName> DefaultIgnoreBaseClasses;
        };

        // Returns the loaded (or default) config.  Thread-safe; loads lazily.
        const GlobalConfig& GetGlobals();

        // Re-reads aimbot.yaml from disk and replaces the live config.
        // Safe to call from any thread (takes an internal mutex).
        void ReloadGlobals();

        // ─────────────────────────────────────────────────────────────────────
        //  Per-weapon overrides
        //
        //  Resolution: weapon-class override → global default. Lists replace
        //  the parent layer entirely (no merging).  Configured via the
        //  'weapons' section of aimbot.yaml (replaces the old aim_config.json).
        // ─────────────────────────────────────────────────────────────────────

        struct WeaponConfigOverride
        {
            std::optional<float> AimbotFOVDeg;
            std::optional<float> SilentAimFOVDeg;
            std::optional<bool>  SilentAimRequireLOS;
            std::optional<std::vector<SDK::FName>> IgnoreBaseClasses;
            std::optional<std::vector<SDK::FName>> ForceIncludeClasses;
            std::optional<std::string> TargetSelector;
        };

        std::unordered_map<std::string, WeaponConfigOverride>& WeaponOverridesRef();
        void EnsureOverridesLoaded();

        float                 ResolveAimbotFOV(SDK::AItem* eq);
        float                 ResolveSilentAimFOV(SDK::AItem* eq);
        bool                  ResolveSilentAimRequireLOS(SDK::AItem* eq);
        Targeting::SelectorFn ResolveTargetSelector(SDK::AItem* eq);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Debug — compile-time diagnostic flags.
    //  Flip and rebuild to enable verbose logging for a specific subsystem.
    // ─────────────────────────────────────────────────────────────────────────

    namespace Debug
    {
        inline constexpr bool LogSilentAim = true;

        inline constexpr bool ShowGetComponentByClassQuery = true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Shared internal types
    // ─────────────────────────────────────────────────────────────────────────

    // Set by OnWeaponFired (Aimbot_Hitscan_Standard.cpp), read by the terrain
    // redirect, multi-pellet optimizer, and projectile activate hooks.
    struct HitscanRedirect
    {
        bool                       active  = false;
        SDK::UPrimitiveComponent*  target  = nullptr;  // mesh component owning the WP
        SDK::UFSDPhysicalMaterial* physMat = nullptr;  // chosen WP phys mat (null for body fallback)
        SDK::FVector               pos     = {};       // target world position
    };
    extern HitscanRedirect g_PendingRedirect;

    struct AimTargetInfo
    {
        SDK::FVector                  pos     = {};
        SDK::AFSDPawn*                enemy   = nullptr;
        SDK::USkeletalMeshComponent*  mesh    = nullptr;   // mesh owning the chosen WP (for Server_RegisterHit)
        SDK::UFSDPhysicalMaterial*    physMat = nullptr;   // chosen WP phys mat (null for body-center fallback)
    };

    struct BestDamageBody
    {
        SDK::FVector                  pos        = {};
        SDK::USkeletalMeshComponent*  mesh       = nullptr;
        SDK::UFSDPhysicalMaterial*    physMat    = nullptr;
        float                         multiplier = 0.f;
        SDK::FName                    boneName{};   // for Server_RegisterHit_Destructable BoneIndex
    };

    struct BreakableWpState
    {
        bool  isBreakable = false;
        float health      = -1.f;
        bool  isDestroyed = false;
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Internal function declarations
    //  All definitions are in Aimbot_TargetSelection.cpp unless noted.
    // ─────────────────────────────────────────────────────────────────────────

    // GetMesh() on AEnemyPawn is a BlueprintImplementableEvent with no C++ body.
    // Use K2_GetComponentsByClass for AEnemyPawn; direct field for ADeepPathfinderCharacter.
    std::vector<SDK::USkeletalMeshComponent*> GetEnemyMeshes(SDK::AFSDPawn* Enemy);

    bool IsWeakpointVisible(SDK::APlayerCharacter* LocalPlayer, SDK::AFSDPawn* Enemy,
        const SDK::FVector& CamLoc, const SDK::FVector& WPos, SDK::FName BoneName);

    BreakableWpState GetBreakableWpState(SDK::AFSDPawn* Enemy, SDK::USkeletalMeshComponent* Mesh,
        SDK::FName BoneName, SDK::UFSDPhysicalMaterial* PhysMat);

    std::optional<AimTargetInfo> FindAimbotTarget(SDK::APlayerCharacter* LocalPlayer,
        SDK::APlayerCameraManager* CamMgr,
        const SDK::FVector& CamLoc,
        const SDK::FVector& Forward,
        float FOVDeg,
        bool RequireLOS = true);

    std::optional<SDK::FVector> FindAimbotTargetPos(SDK::APlayerCharacter* LocalPlayer,
        SDK::APlayerCameraManager* CamMgr,
        const SDK::FVector& CamLoc,
        const SDK::FVector& Forward,
        float FOVDeg,
        bool RequireLOS = true);

    std::optional<BestDamageBody> FindBestDamageBody(SDK::AFSDPawn* Enemy, float MinMultiplier);

    // Aimbot_DebugConfig.cpp
    void WPInfo(const CommandContext& ctx);
    void ToggleFireSpy();

    // ─────────────────────────────────────────────────────────────────────────
    //  Silent aim subsystem — Enable/Disable pairs called by ToggleSilentAim.
    //  Each pair is defined in its corresponding Aimbot_*.cpp file.
    // ─────────────────────────────────────────────────────────────────────────

    // Aimbot_Hitscan_Standard.cpp
    void EnableHitscanSilentAim();         void DisableHitscanSilentAim();
    void EnableHitscanRpcRedirect();       void DisableHitscanRpcRedirect();
    void EnableHitscanHitOptimize();       void DisableHitscanHitOptimize();

    // Aimbot_Hitscan_Multi.cpp
    void EnableHitscanMultiOptimize();     void DisableHitscanMultiOptimize();

    // Aimbot_Hitscan_Destructable.cpp
    void EnableHitscanDestructableOptimize(); void DisableHitscanDestructableOptimize();

    // Aimbot_Hitscan_Capsule.cpp
    void EnableHitscanCapsuleOptimize();      void DisableHitscanCapsuleOptimize();

    // Aimbot_Hitscan_Reflection.cpp
    void EnableHitscanReflectionOptimize();   void DisableHitscanReflectionOptimize();

    // Aimbot_Projectile_ServerFire.cpp
    void EnableProjectileSilentAim();      void DisableProjectileSilentAim();

    // Aimbot_Projectile_Activate.cpp
    void EnableProjectileActivate();       void DisableProjectileActivate();

} // namespace AimAssist
