// Aim.cpp — implementation split into Aimbot_*.cpp files.
//
// Files and their responsibilities:
//   Aim_Internal.h              — shared types, Config/Debug namespaces, internal declarations
//   Aimbot_Config.cpp           — per-weapon override storage, JSON loading, resolve helpers
//   Aimbot_TargetSelection.cpp  — selector registry, enemy mesh traversal, FindAimbotTarget
//   Aimbot_DebugConfig.cpp      — wpinfo command, firespy toggle
//   Aimbot_Base.cpp             — public state, RCS loop, ToggleSilentAim, command registration
//   Aimbot_Hitscan_Standard.cpp — UHitscanComponent: OnWeaponFired + Server_RegisterHit + _Terrain
//                                 NOTE: UAllPiercingHitscanComponent inherits UHitscanComponent with
//                                 no own RPCs — covered here by ExactOrSubclass matching.
//   Aimbot_Hitscan_Multi.cpp        — UMultiHitscanComponent: Server_RegisterHit (shotguns)
//   Aimbot_Hitscan_Capsule.cpp      — UCapsuleHitscanComponent: Server_RegisterHit (capsule sweep)
//   Aimbot_Hitscan_Reflection.cpp   — UReflectionHitscanComponent: Server_RegisterHit + _Reflection
//   Aimbot_Hitscan_Destructable.cpp — UHitscanComponent: Server_RegisterHit_Destructable
//   Aimbot_Projectile_ServerFire.cpp — UProjectileLauncherBaseComponent: Server_Fire
//   Aimbot_Projectile_Activate.cpp   — AProjectileBase: Activate
