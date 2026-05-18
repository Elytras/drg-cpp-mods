// Commands.cpp — All game command implementations and callback handlers.
#include "Commands.h"
#include "ModManager.h"
#include "Library.h"
#include "Common.h"
#include "Lib_Utils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <random>
#include <fstream>
#include <filesystem>
#include <thread>
#include <numbers>
#include <type_traits>
#include <nlohmann/json.hpp>

#include "SDK/SDK/Basic.hpp"
#include "SDK/SDK/CoreUObject_classes.hpp"
#include "SDK/SDK/CoreUObject_structs.hpp"
#include "SDK/SDK/FSD_classes.hpp"
#include "SDK/SDK/FSDEngine_classes.hpp"
#include "SDK/SDK/Engine_classes.hpp"
#include "SDK/SDK/JSONValue_classes.hpp"
#include "SDK/SDK/JSON_classes.hpp"
#include "SDK/SDK/JSONStream_classes.hpp"
#include "SDK/SDK/JSON_parameters.hpp"
#include "SDK/SDK/CD2_Mod_classes.hpp"
//#include "SDK/SDK/BP_BhaBarnacleWorm_classes.hpp"
#include "SDK/SDK/LIB_Game_classes.hpp"
#include "SDK/SDK/Engine_parameters.hpp"
#include "SDK/SDK/BP_LockOnRifle_AoE_classes.hpp"
#include "SDK/SDK/FSD_parameters.hpp"
#include "SDK/SDK/WPN_LockOnRifle_classes.hpp"
#include "SDK/SDK/ReplicatedActor_classes.hpp"
#include "SDK/SDK/ReplicatedActor_functions.cpp"
#pragma push_macro("EOF")
#undef EOF

extern HANDLE g_hRespEvent; //Otherwise the call stuff doesn't work

// =========================================================================
// Compile-time configuration
// =========================================================================
constexpr bool CheckLocal = true;
constexpr bool DeleteOnTwerk = false;
constexpr bool VoidTwerkPE = false;
constexpr bool KillOnTwerk = true;

using namespace l;
using namespace SDK::Params;
using namespace ObjectCast;
using namespace GameHooks;
using namespace VarSystem;

using ctx = CommandContext;
// =========================================================================
// Forward Declarations
// =========================================================================

namespace Weapons
{
    std::vector<SDK::AAmmoDrivenWeapon*> GetAllAmmoWeapons(APlayerCharacter* LocalPlayer);
    std::vector<SDK::AAmmoDrivenWeapon*> GetTrackedAmmoWeapons();
    void LokiSphereExplosions(AWPN_LockOnRifle_C* Loki, float R, int N);
    void UpgradeLoki(AWPN_LockOnRifle_C* Loki);
    void NukeEnemiesWithECR(AWPN_LockOnRifle_C* Loki);
    void ECRTrail(AWPN_LockOnRifle_C* Loki, size_t Count);
}
// =========================================================================
// Helpers
// =========================================================================

static std::ofstream OpenOutput(const std::string& path)
{
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(path).parent_path());
    try {
        fs::remove(path);
    }
    catch (const fs::filesystem_error& e) {
        error("Failed to remove existing file: %s", e.what());
    }
    return std::ofstream(path);
}

inline nlohmann::json ExtractNumericPropertiesAsJson(uintptr_t Base, UClass* StopAt, const std::string& prefix = "")
{
    nlohmann::json result = nlohmann::json::object();
    if (!Base) return result;

    const auto chain = BuildClassChain(reinterpret_cast<UObject*>(Base)->Class, StopAt);
    for (UStruct* level : chain)
    {
        UClass* cls = ObjectCast::Cast<UClass>(level);
        if (!cls) continue;

        for (FField* field : FFieldRange(cls->ChildProperties))
        {
            bool numeric = false;
            std::string typePrefix;

            FieldCast::Visit(field, [&](auto* p)
                {
                    using T = std::remove_pointer_t<decltype(p)>;
                    if (std::is_same_v<T, FFloatProperty>) { numeric = true; typePrefix = "Float"; }
                    else if (std::is_same_v<T, FDoubleProperty>) { numeric = true; typePrefix = "Float"; }
                    else if (std::is_same_v<T, FBoolProperty>) { numeric = true; typePrefix = "Bool"; }
                    else if (std::is_same_v<T, FIntProperty>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, FInt8Property>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, FInt16Property>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, FInt64Property>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, FUInt16Property>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, FUInt32Property>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, FUInt64Property>) { numeric = true; typePrefix = "Int"; }
                });

            if (!numeric) continue;

            std::string fieldName = field->Name.ToString();
            std::string fullKey = typePrefix + ":" + prefix + fieldName;
            result[fullKey] = GetFieldValueAsString(Base, field);
        }
    }

    return result;
}

static inline const bool CanChangeWorld() {
    return IsValidOf<APlayerCharacter>(GetLocalPlayer());
}

namespace Enum {
    constexpr int8 EnemyHealthIndex(EEnemyHealthScaling Scale)
    {
        switch (Scale)
        {
        case EEnemyHealthScaling::SmallEnemy:      return 0;
        case EEnemyHealthScaling::LargeEnemy:      return 1;
        case EEnemyHealthScaling::ExtraLargeEnemy: return 2;
        case EEnemyHealthScaling::ExtraLargeEnemyB:return 3;
        case EEnemyHealthScaling::ExtraLargeEnemyC:return 4;
        case EEnemyHealthScaling::ExtraLargeEnemyD:return 5;
        case EEnemyHealthScaling::NoScaling:       return 6;
        default: return -1; // error
        }
    }
}

namespace Helpers {

    struct FDamageInfo
    {
        float Damage = 0.f;
        float RadialDamage = 0.f;

        bool IsValid() const
        {
            return Damage > 0.001f || RadialDamage > 0.01f;
        }
    };

    static bool ExtractDamageInfo(
        UDamageComponent* DamageComponent,
        FDamageInfo& OutInfo)
    {
        if (!DamageComponent)
            return false;

        OutInfo.Damage = DamageComponent->Damage;
        OutInfo.RadialDamage = DamageComponent->RadialDamage;

        return true;
    }

    static bool GetDamageInfoFromProjectileClass(
        UClass* ProjectileClass,
        FDamageInfo& OutInfo)
    {
        if (!ProjectileClass)
            return false;

        AProjectileBase* ProjectileCDO =
            Cast<AProjectileBase>(ProjectileClass->ClassDefaultObject);

        if (!ProjectileCDO)
            return false;

        // Try CDO first
        if (ExtractDamageInfo(
            Cast<UDamageComponent>(
                ProjectileCDO->GetComponentByClass(
                    UDamageComponent::StaticClass())),
            OutInfo))
        {
            return true;
        }

        // Runtime-created components fallback
        FTransform Transform{};
        Transform.Translation = FVector{ 9999999,9999999,9999999 };

        AProjectileBase* SpawnedProjectile = nullptr;

        if (!SpawnActor<AProjectileBase>(
            ProjectileClass,
            Transform,
            SpawnedProjectile))
        {
            return false;
        }

        bool bResult = ExtractDamageInfo(
            Cast<UDamageComponent>(
                SpawnedProjectile->GetComponentByClass(
                    UDamageComponent::StaticClass())),
            OutInfo);

        SpawnedProjectile->K2_DestroyActor();

        return bResult;
    }
    // GetMesh() on AEnemyPawn is a BlueprintImplementableEvent with no C++ body — always null.
    // Use K2_GetComponentsByClass for AEnemyPawn; direct field for ADeepPathfinderCharacter.
    static std::vector<USkeletalMeshComponent*> GetEnemyMeshes(AFSDPawn* Enemy)
    {
        using namespace ObjectCast;

        if (auto* dp = Cast<AEnemyDeepPathfinderCharacter>(Enemy))
            return dp->Mesh ? std::vector<USkeletalMeshComponent*>{ dp->Mesh }
                            : std::vector<USkeletalMeshComponent*>{};

        std::vector<USkeletalMeshComponent*> result;
        auto comps = Enemy->K2_GetComponentsByClass(USkeletalMeshComponent::StaticClass());
        for (int32 i = 0; i < comps.Num(); ++i)
            if (auto* smc = Cast<USkeletalMeshComponent>(comps[i]))
                result.push_back(smc);
        return result;
    }

    // Largest sphere/capsule radius from the body's aggregate geometry.
    // Fallback 15 UU (~15 cm) for bodies with no sphere or sphyl shapes.
    static float GetBodyRadius(USkeletalBodySetup* Body)
    {
        float r = 0.f;
        for (int32 i = 0; i < Body->AggGeom.SphereElems.Num(); ++i)
            r = std::max(r, Body->AggGeom.SphereElems[i].Radius);
        for (int32 i = 0; i < Body->AggGeom.SphylElems.Num(); ++i)
            r = std::max(r, Body->AggGeom.SphylElems[i].Radius);
        return r > 0.f ? r * 0.75f : 15.f;
    }

    // Returns true if the weakpoint physics body identified by BoneName is visible from CamLoc.
    // Uses physics-body trace (bTraceComplex=false) so FHitResult::BoneName is populated,
    // allowing a direct name match instead of a fragile distance threshold.
    static bool IsWeakpointVisible(APlayerCharacter* LocalPlayer, AFSDPawn* Enemy,
                                   const FVector& CamLoc, const FVector& WPos, FName BoneName)
    {
        TArray<AActor*> NoIgnore;
        FHitResult Hit;
        const bool bHit = UKismetSystemLibrary::LineTraceSingle(
            LocalPlayer, CamLoc, WPos,
            ETraceTypeQuery::TraceTypeQuery1, false,  // false = physics bodies, populates BoneName
            NoIgnore, EDrawDebugTrace::None,
            &Hit, true,                               // bIgnoreSelf — skips LocalPlayer pawn
            FLinearColor{}, FLinearColor{}, 0.f);

        if (!bHit) return true;                                           // clear path
        if (Hit.Actor.Get() != static_cast<AActor*>(Enemy)) return false; // terrain or other actor
        return Hit.BoneName == BoneName;                                  // hit THIS weakpoint body
    }

    // ── Breakable weakpoint / armor state ────────────────────────────────────────────────

    struct BreakableWpState
    {
        bool  isBreakable = false;
        float health      = -1.f;   // -1 = no per-bone health (SimpleArmor or unmatched)
        bool  isDestroyed = false;
    };

    // UE4 4.27 TMap AllocationFlags: inline uint32[4] starts at map_base+0x10.
    // Returns whether slot i contains a live element.
    static bool TMapSlotValid(const void* m, int32 i)
    {
        if (i < 0 || i >= 128) return false;
        const auto* f = reinterpret_cast<const uint32*>(static_cast<const uint8*>(m) + 0x10);
        return (f[i >> 5] >> (i & 31)) & 1u;
    }

    // Given a weakpoint physics body (bone + material) on a specific mesh component,
    // finds the UBaseArmorDamageComponent that manages it and returns its health/broken state.
    // Returns isBreakable=false if no armor component matches (non-breakable weakpoint).
    //
    // TMap<FName(8), Value(0x18)> element stride: sizeof(TSetElement<TPair<K,V>>) = 40
    //   slot+0x00: FName key (8 bytes)
    //   slot+0x08: Value (24 bytes)
    //   slot+0x20: HashNextId (4 bytes)
    //   slot+0x24: HashIndex (4 bytes)
    static BreakableWpState GetBreakableWpState(
        AFSDPawn* Enemy, USkeletalMeshComponent* Mesh,
        FName BoneName, UFSDPhysicalMaterial* PhysMat)
    {
        using namespace ObjectCast;
        BreakableWpState r;

        auto comps = Enemy->K2_GetComponentsByClass(UBaseArmorDamageComponent::StaticClass());
        for (int32 ci = 0; ci < comps.Num(); ++ci)
        {
            auto* base = Cast<UBaseArmorDamageComponent>(comps[ci]);
            if (!base || base->Mesh != Mesh) continue;

            bool manages = false;
            for (int32 mi = 0; mi < base->ArmorPhysMats.Num(); ++mi)
                if (base->ArmorPhysMats[mi] == PhysMat) { manages = true; break; }
            if (!manages) continue;

            r.isBreakable = true;

            if (auto* hc = Cast<UArmorHealthDamageComponent>(base))
            {
                const void*  mp     = &hc->PhysBoneToArmor;
                const int32  nSlots = *reinterpret_cast<const int32*>(static_cast<const uint8*>(mp) + 0x08);
                const uint8* buf    = *reinterpret_cast<uint8* const*>(mp);
                if (!buf) break;

                for (int32 i = 0; i < nSlots; ++i)
                {
                    if (!TMapSlotValid(mp, i)) continue;
                    const uint8*            slot  = buf + i * 40;
                    const FArmorHealthItem* item  = reinterpret_cast<const FArmorHealthItem*>(slot + 8);
                    const bool              byMask = (hc->ArmorDamageInfo.ArmorIndexMask >> item->MaterialIndex) & 1;

                    for (int32 bi = 0; bi < item->ArmorBones.Num(); ++bi)
                    {
                        const FArmorHealthSubItem& sub = item->ArmorBones[bi];
                        bool match = (sub.BoneName == BoneName);
                        if (!match)
                            for (int32 ai = 0; !match && ai < sub.AdditionalBones.Num(); ++ai)
                                if (sub.AdditionalBones[ai] == BoneName) match = true;
                        if (!match) continue;
                        r.health      = sub.Health;
                        r.isDestroyed = byMask || sub.Health <= 0.f;
                        return r;
                    }
                }
            }
            else if (auto* sc = Cast<USimpleArmorDamageComponent>(base))
            {
                const void*  mp     = &sc->PhysBoneToArmor;
                const int32  nSlots = *reinterpret_cast<const int32*>(static_cast<const uint8*>(mp) + 0x08);
                const uint8* buf    = *reinterpret_cast<uint8* const*>(mp);
                if (!buf) break;

                for (int32 i = 0; i < nSlots; ++i)
                {
                    if (!TMapSlotValid(mp, i)) continue;
                    const uint8*                    slot = buf + i * 40;
                    const FDestructableBodypartItem* item = reinterpret_cast<const FDestructableBodypartItem*>(slot + 8);

                    for (int32 bi = 0; bi < item->ArmorBones.Num(); ++bi)
                    {
                        if (item->ArmorBones[bi] != BoneName) continue;
                        r.isDestroyed = (sc->ArmorDamageInfo.ArmorIndexMask >> item->MaterialIndex) & 1;
                        return r;
                    }
                }
            }
            break; // found managing component
        }
        return r;
    }

    const std::array<float, static_cast<int>(EEnemyHealthScaling::EEnemyHealthScaling_MAX)> GetCurrentEnemyHealthScalings() {
        std::array<float, static_cast<int>(EEnemyHealthScaling::EEnemyHealthScaling_MAX)> Scalings{};

        UDifficultySetting* Difficulty = CastChecked<AFSDGameState>(UGameplayStatics::GetGameState(GetWorld()))->GetCurrentDifficultySetting();
        int32 Idx = GameLib::GetNumPlayers(GetWorld(), false) >= 4 ? 3 : GameLib::GetNumPlayers(GetWorld(), false);

        using namespace Enum;
        using enum EEnemyHealthScaling;

        Scalings[EnemyHealthIndex(SmallEnemy)] = Difficulty->SmallEnemyDamageResistance[Idx];
        Scalings[EnemyHealthIndex(LargeEnemy)] = Difficulty->EnemyDamageResistance[Idx];
        Scalings[EnemyHealthIndex(ExtraLargeEnemy)] = Difficulty->ExtraLargeEnemyDamageResistance[Idx];
        Scalings[EnemyHealthIndex(ExtraLargeEnemyB)] = Difficulty->ExtraLargeEnemyDamageResistanceB[Idx];
        Scalings[EnemyHealthIndex(ExtraLargeEnemyC)] = Difficulty->ExtraLargeEnemyDamageResistanceC[Idx];
        Scalings[EnemyHealthIndex(ExtraLargeEnemyD)] = Difficulty->ExtraLargeEnemyDamageResistanceD[Idx];
        Scalings[EnemyHealthIndex(NoScaling)] = 1;

        return Scalings;
    }
}
// =========================================================================
// State
// =========================================================================
namespace State
{
    CallbackHandle LogAllEventsCallback = 0;
    CallbackHandle DanceCallback = 0;
    CallbackHandle TickCallback = 0;
    CallbackHandle ClientChatCallback = 0;
    CallbackHandle ServerChatCallback = 0;
    CallbackHandle ProxyModCallback = 0;
    CallbackHandle BeginPlayCallback = 0;

    bool infiniteAmmoEnabled = false;
    std::unordered_map<UObject*, int> OldShotCost;

    bool   RecoilEnabled    = false;
    bool   RCSInitialized   = false;
    float  RCSDesiredPitch  = 0.f;
    float  RCSPrevCtrlPitch = 0.f;
    float  RCSDesiredYaw    = 0.f;
    float  RCSPrevCtrlYaw   = 0.f;

    bool           SilentAimEnabled   = false;
    CallbackHandle SilentAimHandle    = 0;
    bool           AimbotEnabled      = false;
    bool           AimbotHasTarget    = false;
    CallbackHandle AimbotHandle       = 0;

    struct ScannedFunction
    {
        UFunction* Func = nullptr;
        UObject* Owner = nullptr;
        std::string FunctionName;
        std::string OwnerName;
        std::string OwnerClassName;
        std::string ExplicitName;
    };
    std::unordered_map<std::string, ScannedFunction> ScannedFunctions;
    std::unordered_map<std::string, std::vector<std::string>> ScannedFunctionVariantsByName;

    const auto dummyVec = std::vector<std::string>{};
    const auto dummyCtx = CommandContext{ dummyVec };
}

// =========================================================================
// Tick system
// =========================================================================
namespace TickSystem
{
    constexpr long double kMinTickIntervalMs = 0.00001L;

    long double NormalizeIntervalMs(long double intervalMs)
    {
        return std::max(intervalMs, kMinTickIntervalMs);
    }

    long double NormalizeFrequencyHz(long double frequencyHz)
    {
        return std::max(frequencyHz, kMinTickIntervalMs);
    }

    template <typename TRate>
    std::function<long double()> MakeIntervalProvider(TRate&& rate)
    {
        using RateType = std::decay_t<TRate>;

        if constexpr (std::is_invocable_v<RateType>)
        {
            return [rate = std::forward<TRate>(rate)]()
                {
                    return NormalizeIntervalMs(static_cast<long double>(rate()));
                };
        }
        else
        {
            const long double intervalMs = NormalizeIntervalMs(static_cast<long double>(rate));
            return [intervalMs]() { return intervalMs; };
        }
    }

    template <typename TRate>
    std::function<long double()> MakeIntervalProviderFromFrequency(TRate&& rate)
    {
        using RateType = std::decay_t<TRate>;

        if constexpr (std::is_invocable_v<RateType>)
        {
            return [rate = std::forward<TRate>(rate)]()
                {
                    return 1000.0L / NormalizeFrequencyHz(static_cast<long double>(rate()));
                };
        }
        else
        {
            const long double intervalMs =
                1000.0L / NormalizeFrequencyHz(static_cast<long double>(rate));
            return [intervalMs]() { return intervalMs; };
        }
    }

    struct TickEntry
    {
        CallbackHandle        handle;
        std::function<void()> fn;
        std::function<long double()> intervalMsProvider;
        long double           accumulatedMs;
    };

    std::vector<TickEntry> g_TickFunctions;

    // Called directly from ModManager::UpdateGameThread
    void Dispatch(long double actualDeltaMs)
    {
        if (actualDeltaMs > 500.0L) actualDeltaMs = 500.0L;

        for (auto& entry : g_TickFunctions)
        {
            const long double intervalMs =
                entry.intervalMsProvider
                ? NormalizeIntervalMs(entry.intervalMsProvider())
                : kMinTickIntervalMs;

            entry.accumulatedMs += actualDeltaMs;
            if (entry.accumulatedMs >= intervalMs)
            {
                entry.accumulatedMs -= intervalMs;
                if (entry.fn) entry.fn();
            }
        }
    }

    template <typename TIntervalRate>
    CallbackHandle SetTickableFunction_AsIntervalMs(
        std::function<void()> fn,
        TIntervalRate&& intervalRate)
    {
        static CallbackHandle nextHandle = 1000;
        CallbackHandle handle = nextHandle++;

        auto intervalMsProvider = MakeIntervalProvider(std::forward<TIntervalRate>(intervalRate));

        // Enqueue so registration is always game-thread-safe
        GameHooks::ProcessEventHook::Get().Enqueue(
            [fn = std::move(fn), handle, intervalMsProvider = std::move(intervalMsProvider)]() mutable
            {
                g_TickFunctions.push_back({ handle, std::move(fn), std::move(intervalMsProvider), 0.L });
            });

        return handle;
    }

    template <typename TFrequencyRate>
    CallbackHandle SetTickableFunction_AsFrequencyHz(
        std::function<void()> fn,
        TFrequencyRate&& frequencyRate)
    {
        return SetTickableFunction_AsIntervalMs(
            std::move(fn),
            MakeIntervalProviderFromFrequency(std::forward<TFrequencyRate>(frequencyRate)));
    }

    void ClearTickableFunction(CallbackHandle handle)
    {
        GameHooks::ProcessEventHook::Get().Enqueue([handle]()
            {
                std::erase_if(g_TickFunctions,
                    [handle](const TickEntry& e) { return e.handle == handle; });
            });
    }

    void ClearAllTickableFunctions()
    {
        GameHooks::ProcessEventHook::Get().Enqueue([]()
            {
                g_TickFunctions.clear();
            });
    }

    void Reset()
    {
        g_TickFunctions.clear();
    }
}

// =========================================================================
// Twerker helpers
// =========================================================================
namespace Twerking
{
    struct TrailRateState
    {
        bool bExisted = false;
        long double smoothedSpeed = 0.0L;
        long double smoothedDistance = 0.0L;
    };

    inline TrailRateState g_SpawnTwerkTrailState{};

    static long double GetSpawnTwerkTrailFrequencyHz()
    {
        // Compile-time configuration
        constexpr bool bUseVelocity = true;
        constexpr long double spacingUnits = 15.0L;
        constexpr long double minHz = 0.01L;
        constexpr long double maxHz = 60.0L;
        constexpr long double alpha = 0.2L; // Smoothing factor (20% new, 80% old)
        constexpr long double epsilon = 0.01L; // Threshold to consider as "stopped"
        constexpr long double defaultHz = 0.2L;

        auto* player = GetLocalPlayer();
        static FVector lastPosition{};
        auto& s = g_SpawnTwerkTrailState;

        if (!IsValidOf<APlayerCharacter>(player) || !GetWorld()) {
            s.bExisted = false;
            return defaultHz;
        }

        const FVector currentPosition = player->K2_GetActorLocation();

        if (!s.bExisted) {
            lastPosition = currentPosition;
            s.bExisted = true;
            s.smoothedDistance = 0.0L;
            s.smoothedSpeed = 0.0L;
            return defaultHz;
        }

        long double calculatedHz = 0.0L;

        if constexpr (bUseVelocity) {
            const long double speed = (long double)player->GetVelocity().Size();

            s.smoothedSpeed = (s.smoothedSpeed * (1.0L - alpha)) + (speed * alpha);
            calculatedHz = s.smoothedSpeed / spacingUnits * 0.1L;

            //info("TwerkTrail [Vel] | Speed: {:.2f} | Hz: {:.2f}", (double)speed, (double)calculatedHz);
        }
        else {
            // --- Displacement Branch ---
            const long double distance = (long double)FVector::Dist(currentPosition, lastPosition);

            s.smoothedDistance = (s.smoothedDistance * (1.0L - alpha)) + (distance * alpha);
            calculatedHz = s.smoothedDistance / (spacingUnits * 0.1L);

            //info("TwerkTrail [Dist] | Dist: {:.2f} | Hz: {:.2f}", (double)distance, (double)calculatedHz);
        }

        // Always update lastPosition for the next frame
        lastPosition = currentPosition;

        return std::clamp(calculatedHz, minHz, maxHz);
    }
}
// =========================================================================
// Tickable functions (game logic)
// =========================================================================

namespace Tickables
{
    void SpawnTwerk() {
        if (!GetWorld()) return;
        auto* Player = GetLocalPlayer();
        if (!IsValidOf<APlayerCharacter>(Player)) return;
        if (!IsOnSpacerig()) return;
        Player->Server_CheatDancingCharacterOnSelf(18);
    }
    void TickSpam()
    {
        static int counter = 0;
        ++counter;
        auto* Player = GetLocalPlayer();
        if (!Player || !Kismet::IsValid(Player)) return;
        auto ChatStr = FString(ToWide(std::format("Tick {}", counter)).c_str());
        Player->GetPlayerController()->Server_NewMessage(
            Player->GetPlayerState()->GetPlayerName(), ChatStr,
            Player->GetPlayerState()->GetChatSenderType());
    }
    void Rotator() {
        static long double offset = 0;
        offset += 0.001;
    }

    void DeletePitjaws() {
        if (!Kismet::IsServer(nullptr)) return;
        if (!IsValidOf<APlayerCharacter>(GetLocalPlayer())) return;
        TArray<AActor*> FoundActors;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), APitJaw::StaticClass(), &FoundActors);
        for (AActor* Actor : FoundActors)
            if (IsValidOf<APitJaw>(Actor)) Actor->K2_DestroyActor();
    }

    void FearAura() {
        ACoilGun* CoilGun = GetSecondaryWeapon<ACoilGun>(GetLocalPlayer());
        if (!IsValidOf<ACoilGun>(CoilGun)) return;

        TArray<AActor*> FoundActors;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AFSDPawn::StaticClass(), &FoundActors);
        if (FoundActors.Num() == 0) return;

        if (Kismet::IsServer(GetWorld()))
        {
            for (AActor* Actor : FoundActors)
                if (IsValidOf<AActor>(Actor))
                    CoilGun->Server_FearTarget(Actor);

            return;
        }

        std::vector<AFSDPawn*> Pawns;
        Pawns.reserve(FoundActors.Num());

        for (AActor* Actor : FoundActors)
            if (AFSDPawn* Pawn = Cast<AFSDPawn>(Actor))
                if (Pawn->GetAttitude() != EPawnAttitude::Friendly)
                    Pawns.push_back(Pawn);

        EnqueueThrottled<AFSDPawn*>(std::move(Pawns), std::chrono::milliseconds(16),
            [CoilGun](AFSDPawn* Target, size_t Index, size_t Total) -> bool
            {
                if (IsValidOf<ACoilGun>(CoilGun) && IsValidOf<AFSDPawn>(Target)) CoilGun->Server_FearTarget(Target);
                return true;
            });
    }

    void CoilResistance() {
        ACoilGun* CoilGun = GetSecondaryWeapon<ACoilGun>(GetLocalPlayer());
        if (!IsValidOf<ACoilGun>(CoilGun)) return;
        CoilGun->Server_ToggleCharingBonuses(true);
    }

    void LokiAbuse()
    {
        AWPN_LockOnRifle_C* Loki = GetPrimaryWeapon<AWPN_LockOnRifle_C>(GetLocalPlayer());
        if (!IsValidOf<AWPN_LockOnRifle_C>(Loki)) return;
        Weapons::UpgradeLoki(Loki);
        Weapons::NukeEnemiesWithECR(Loki);
        //Weapons::ECRTrail(Loki, 128);
        //Weapons::LokiSphereExplosions(Loki, 1000, 1024);
    }

    void LockPlayers() {
        static const std::vector<std::string> playersToLock = { "29 Phantom" };
        auto Local = GetLocalPlayer();
        if (!GetWorld() && !GetWorld()->GetName().contains("LVL_SpaceRig")) return;
        if (!IsValidOf<APlayerCharacter>(Local)) return;
        AReplicatedActor_C* ActualTeleporter = nullptr;
        TArray<AActor*> Teleporters{};
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AReplicatedActor_C::StaticClass(), &Teleporters);
        for (auto* T : Teleporters)
            if (IsValidOf<AReplicatedActor_C>(T) && T->GetOwner() == Local)
            {
                ActualTeleporter = static_cast<AReplicatedActor_C*>(T); break;
            }
        if (!IsValidOf<AReplicatedActor_C>(ActualTeleporter)) return;
        TArray<APlayerCharacter*> Players = ActorLib::GetAllPlayerCharacters(nullptr);
        std::vector<APlayerCharacter*> Targets;
        for (auto P : Players) {
            if (IsValidOf<APlayerCharacter>(P) &&
                std::find(
                    playersToLock.begin(),
                    playersToLock.end(),
                    P->GetPlayerState()->GetPlayerName().ToString().c_str()) != playersToLock.end())
                Targets.push_back(P);
        }
        if (Targets.empty()) return;
        EnqueueThrottled<APlayerCharacter*>(Targets, std::chrono::milliseconds(20),
            [ActualTeleporter](APlayerCharacter* Target, size_t Index, size_t Total) -> bool
            {
                if (IsValidOf<APlayerCharacter>(Target) && IsValidOf<AReplicatedActor_C>(ActualTeleporter))
                    ActualTeleporter->Server_TeleportPlayer(Target, FVector{});
                return true;
            });
    }
}

// =========================================================================
// Weapon helpers
// =========================================================================
namespace Weapons
{
    std::vector<SDK::AAmmoDrivenWeapon*> GetAllAmmoWeapons(APlayerCharacter* LocalPlayer)
    {
        std::vector<SDK::AAmmoDrivenWeapon*> weapons;
        if (!LocalPlayer || !Kismet::IsValid(LocalPlayer->InventoryComponent)) return weapons;
        for (auto* item : LocalPlayer->InventoryComponent->GetAllItems())
            if (item && item->IsA(SDK::AAmmoDrivenWeapon::StaticClass()))
                weapons.push_back(static_cast<SDK::AAmmoDrivenWeapon*>(item));
        return weapons;
    }

    std::vector<SDK::AAmmoDrivenWeapon*> GetTrackedAmmoWeapons()
    {
        std::vector<SDK::AAmmoDrivenWeapon*> weapons;
        for (auto& [weaponPtr, _] : State::OldShotCost)
            weapons.push_back(static_cast<SDK::AAmmoDrivenWeapon*>(weaponPtr));
        return weapons;
    }

    void LokiSphereExplosions(AWPN_LockOnRifle_C* Loki, float R, int N)
    {
        if (N <= 0 || R <= 0) return;
        if (!IsValidOf<AWPN_LockOnRifle_C>(Loki)) return;
        std::vector<FVector> Points;
        Points.reserve(N);

        const float PHI = 3.14159265359f * (3.0f - std::sqrt(5.0f)); // golden angle
        const float Time = UKismetSystemLibrary::GetGameTimeInSeconds(GetWorld());
        const float RotationSpeed = 1.0f;
        const float TimeOffset = Time * RotationSpeed;
        const FVector PlayerLoc = GetLocalPlayer()->K2_GetActorLocation();

        for (int i = 0; i < N; ++i)
        {
            const float t = (i + 1) / 2.0f;
            const float sign = (i % 2 == 0) ? 1.0f : -1.0f;
            const float z = sign * (t / float((N - 1) / 2));

            const float radius = std::sqrt(1.0f - z * z);

            const float theta = (PHI * i) + TimeOffset;

            const float x = std::cos(theta) * radius;
            const float y = std::sin(theta) * radius;

            Points.push_back(FVector(x, y, z) * R + PlayerLoc);
        }

        EnqueueThrottled<FVector>(
            Points,
            std::chrono::milliseconds(1), // match your other throttled calls
            [Loki](const FVector& Pos, size_t index, size_t total) -> bool
            {
                if (!Loki || !IsValid(Loki)) return false;
                Loki->Server_TriggerAoe(Pos);
                return true;
            }
        );
    }

    void UpgradeLoki(AWPN_LockOnRifle_C* Loki) {
        ASSUME_ASSERT(IsValidOf<AWPN_LockOnRifle_C>(Loki));

        Loki->Resupply(100);
        Loki->bAlwaysHitTarget = true;
        Loki->UseLockOnTargetStatusEffect = true;

        Loki->ShotCost = 0;
        Loki->ReloadDuration = 0.1f;
        Loki->ClipSize = 640;

        Loki->AoeHitCountThreshhold = 1;
        Loki->BurstCount = 64;
        Loki->BurstCycleTime = 0.01f;
        Loki->ChargeSpeed = 200;

        Loki->MaxTargets = 128;
        Loki->MaxLockOnDegree = 30;
        Loki->LoseLockOnDegree = 90;
        Loki->MaxLockOnDuration = 9999.f;
        Loki->MaxLockOnRange = 9999.f;

        Loki->PushStatusEffectEveryXLock = 1;
        Loki->RateOfFire = 200;
        Loki->RateOfFireLockedOnModifier = 200;
    }

    void NukeEnemiesWithECR(AWPN_LockOnRifle_C* Loki)
    {
        ASSUME_ASSERT(IsValidOf<AWPN_LockOnRifle_C>(Loki));
        float damage = 9999999;
        if (auto AoeActor = Loki->AoeActorClass) {
            auto comp = Cast<UDamageComponent>(UActorFunctionLibrary::GetComponentFromClass(Loki->AoeActorClass, UDamageComponent::StaticClass()));
            if (comp) {
                damage = comp->RadialDamage;
                //info("{} {}",ObjToStr(comp), comp->RadialDamage);
            }
        }
        //wtf???
        if (damage <= 0) return;

        auto Scalings = Helpers::GetCurrentEnemyHealthScalings();
        auto Enemies = GetAliveNonFriendlies();
        //info("Found {} enemies", Enemies.size());
        // Call EnqueueThrottled with interval, lambda handles each actor
        EnqueueThrottled<AFSDPawn*>(
            Enemies,
            std::chrono::milliseconds(100), // example throttled interval
            [Loki, damage, Scalings](AFSDPawn* Enemy, size_t index, size_t total) -> bool
            {
                if (!IsValidOf<AWPN_LockOnRifle_C>(Loki) || !IsChildOfByName(Loki, L"WPN_LockOnRifle_C"))
                {
                    warn("Loki became invalid mid run");
                    return false;
                }

                if (!IsValidOf<AFSDPawn>(Enemy) || !IsValidOf<UEnemyHealthComponent>(Enemy->GetHealthComponent())) return true;

                UEnemyHealthComponent* HealthComponent = CastChecked<UEnemyHealthComponent>(Enemy->GetHealthComponent());
                if (!HealthComponent->IsAlive()) return true;
                if (NearlyEqual(Enum::EnemyHealthIndex(HealthComponent->EnemyHealthScaling), -1)) return true;
                float Health = HealthComponent->GetHealth() * Scalings[Enum::EnemyHealthIndex(HealthComponent->EnemyHealthScaling)];
                if (Health <= 0) return true;
                while (Health > 0)
                {
                    Loki->Server_TriggerAoe(Enemy->K2_GetActorLocation());
                    Health -= damage;
                }
                return true;
            }
        );
    }

    void ECRTrail(AWPN_LockOnRifle_C* Loki, size_t Count) {
        if (!IsValidOf<AWPN_LockOnRifle_C>(Loki) || !IsChildOfByName(Loki, L"WPN_LockOnRifle_C"))
            return;

        TArray<APlayerCharacter*> Players = UActorFunctionLibrary::GetAllPlayerCharacters(GetWorld());

        std::vector<int8> dummy(Count);

        EnqueueThrottled<int8>(
            dummy,
            std::chrono::milliseconds(100),
            [Loki](int8, size_t index, size_t total) -> bool {
                if (!IsValidOf<AWPN_LockOnRifle_C>(Loki) || !IsChildOfByName(Loki, L"WPN_LockOnRifle_C"))
                    return false;

                auto Players = UActorFunctionLibrary::GetAllPlayerCharacters(GetWorld());

                for (auto Player : Players) {
                    if (!IsValidOf<APlayerCharacter>(Player) || !IsChildOfByName(Player, L"PlayerCharacter"))
                        continue;

                    float spacing = 150.0f; // distance between trail points

                    FVector pos =
                        Player->K2_GetActorLocation()
                        - Player->GetActorForwardVector() * (spacing * index);

                    Loki->Server_TriggerAoe(pos);
                }

                return true;
            }
        );
    }
}

// =========================================================================
// Scan helpers
// =========================================================================
namespace Scan
{
    std::string BuildExplicitCallName(UObject* Owner, UFunction* Func)
    {
        const std::string ownerClass = Owner && Owner->Class ? Owner->Class->GetName() : "?";
        const std::string ownerName = Owner ? Owner->GetName() : "?";
        const std::string funcName = Func ? Func->GetName() : "?";
        return ownerClass + "::" + ownerName + "::" + funcName;
    }

    std::string BuildFuncSig(UFunction* Func)
    {
        std::string name = Func->GetName();
        struct ParamEntry { std::string text; };
        std::vector<ParamEntry> params;
        params.reserve(8);
        size_t totalParamChars = 0;
        for (auto* field : FFieldRange(Func->ChildProperties))
        {
            if (!FieldCast::IsA<FProperty>(field)) continue;
            auto* Prop = static_cast<FProperty*>(field);
            auto  pflags = static_cast<EPropertyFlags>(Prop->PropertyFlags);
            if (pflags & EPropertyFlags::ReturnParm) continue;
            if (!(pflags & EPropertyFlags::Parm))    continue;
            auto& e = params.emplace_back();
            e.text = Prop->Name.ToString();
            e.text += ": ";
            e.text += GetTypeName(field);
            totalParamChars += e.text.size();
        }
        if (params.empty()) { std::string r = name; r += "()"; return r; }

        size_t singleSize = name.size() + 1 + totalParamChars + (params.size() - 1) * 2 + 1;
        constexpr size_t LineLimit = 80;
        if (singleSize <= LineLimit)
        {
            std::string r = name; r += '(';
            for (size_t i = 0; i < params.size(); ++i) { if (i) r += ", "; r += params[i].text; }
            r += ')'; return r;
        }
        constexpr std::string_view prefix = "║       ", close = "║   )";
        std::string r = name; r += "(\n";
        for (size_t i = 0; i < params.size(); ++i)
        {
            r += prefix; r += params[i].text;
            if (i < params.size() - 1) r += ',';
            r += '\n';
        }
        r += close;
        return r;
    }

    void ScanFunctions(UObject* Obj, std::vector<std::string>& out)
    {
        for (UClass* cls : UClassHierarchyRange(Obj->Class))
            for (auto* field : UFieldRange(cls->Children))
            {
                if (!field->IsA(UFunction::StaticClass())) continue;
                auto* Func = static_cast<UFunction*>(field);
                auto  flags = static_cast<EFunctionFlags>(Func->FunctionFlags);
                if (!(flags & EFunctionFlags::Net))       continue;
                if (!(flags & EFunctionFlags::NetServer)) continue;
                out.push_back(BuildFuncSig(Func));
            }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void DoScan()
    {
        APlayerCharacter* Local = GetLocalPlayer();
        if (!Local) { warn("[scan] No local player."); return; }
        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass(), &AllActors);
        State::ScannedFunctions.clear();
        State::ScannedFunctionVariantsByName.clear();
        int inserted = 0;
        auto TryScanObject = [&](UObject* Obj)
            {
                for (UClass* cls : UClassHierarchyRange(Obj->Class))
                    for (auto* field : UFieldRange(cls->Children))
                    {
                        if (!field->IsA(UFunction::StaticClass())) continue;
                        auto* Func = static_cast<UFunction*>(field);
                        auto  flags = static_cast<EFunctionFlags>(Func->FunctionFlags);
                        if (!(flags & EFunctionFlags::Net))       continue;
                        if (!(flags & EFunctionFlags::NetServer)) continue;
                        std::string name = Func->GetName();
                        std::string explicitName = BuildExplicitCallName(Obj, Func);
                        State::ScannedFunctions[explicitName] = {
                            Func,
                            Obj,
                            name,
                            Obj ? Obj->GetName() : "?",
                            Obj && Obj->Class ? Obj->Class->GetName() : "?",
                            explicitName
                        };
                        State::ScannedFunctionVariantsByName[name].push_back(explicitName);
                        ++inserted;
                    }
            };
        for (auto* Actor : AllActors)
        {
            if (!Actor || !Kismet::IsValid(Actor)) continue;
            if (!Actor->bReplicates)               continue;
            if (NoneOf<AActor*>(Actor->GetOwner(), Local, Local->GetOwner(),
                Local->GetPlayerController(), Local->GetPlayerState())) continue;
            TryScanObject(Actor);
            TArray<UActorComponent*> Components =
                Actor->K2_GetComponentsByClass(UActorComponent::StaticClass());
            for (auto* Comp : Components)
                if (Comp && Kismet::IsValid(Comp)) TryScanObject(Comp);
        }
        for (auto& [name, variants] : State::ScannedFunctionVariantsByName)
        {
            std::sort(variants.begin(), variants.end());
            variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
        }
        info("[scan] Scan complete — {} new functions, {} total.",
            inserted, State::ScannedFunctions.size());
    }
}

// =========================================================================
// ProcessEvent callbacks
// =========================================================================
namespace Callbacks
{
    void LogAllProcessEvents(UObject* Object, UFunction* Function, void*)
    {
        if (!Object)
        {
            trace("[ProcessEvent] UObject=nullptr, Function={}",
                Function ? Function->GetName() : "nullptr");
            return;
        }

        std::string objectName = Object->GetName();
        std::string objectClassName = Object->Class ? Object->Class->GetName() : "?";
        std::string functionName = Function ? Function->GetName() : "nullptr";

        trace("[ProcessEvent] {}::{} -> {}", objectClassName, objectName, functionName);
    }

    void TickListener(UObject* Object, UFunction* Function, void*)
    {
        static UObject* TrackedObject = nullptr;
        if (!TrackedObject || !IsValidRaw(TrackedObject)) TrackedObject = Object;
        if (Object != TrackedObject) return;
        info("Tick: {} -> {}", Object->GetName(), Function ? Function->GetName() : "None");
    }

    void DanceIntercept(UObject* Object, UFunction*, void* Params)
    {
        auto* Args = static_cast<SDK::Params::PlayerCharacter_Server_SetIsDancing*>(Params);

        auto HandlePlayer = [&](APlayerCharacter* Local)
            {
                if (Object == Local) return;
                if (Args->danceMove_0 != 18) return;
                auto* Player = static_cast<APlayerCharacter*>(Object);

                if constexpr (DeleteOnTwerk)
                {
                    info("DanceIntercept: Destroying '{}'", Player->PlayerState->GetPlayerName().ToString());
                    Player->K2_DestroyActor();
                }
                else if constexpr (VoidTwerkPE)
                {
                    info("DanceIntercept: Voiding PE for '{}'", Player->PlayerState->GetPlayerName().ToString());
                    GameHooks::ProcessEventHook::Get().SetExecutionMode(
                        State::DanceCallback, GameHooks::ExecutionMode::SkipOriginal);
                }
                else
                {
                    Player->GetPlayerController()->Server_NewMessage(
                        Player->PlayerState->GetPlayerName(),
                        FString(L"Six Seven"), EChatSenderType::DeluxUser);
                    Args->danceMove_0 = -1;
                    Args->isDancing_0 = false;
                }
            };

        if constexpr (CheckLocal) HandlePlayer(GetLocalPlayer());
        else                      HandlePlayer(GetLocalPlayerCharacterBlocking(5));
    }

    void ServerMessageIntercept(UObject*, UFunction*, void* Params)
    {
        auto& msg = static_cast<SDK::Params::FSDGameState_ClientNewMessage*>(Params)->Msg;
        if (msg.Msg.ToString() == "Six Seven") return;
        const auto& senderName = msg.Sender.ToString();
        const auto& messageText = msg.Msg.ToString();
        if (senderName.empty() && messageText.empty()) return;
        if (senderName.empty()) {
            info("[Chat] {}", messageText);
            return;
        }
        if (messageText.empty()) {
            info("[Chat] {}", senderName);
            return;
        }
        info("[Chat] {}: {}", msg.Sender.ToString(), msg.Msg.ToString());
    }

    void MessageIntercept(UObject*, UFunction*, void* Params)
    {
        auto* msg = static_cast<SDK::Params::FSDPlayerController_Server_NewMessage*>(Params);
        if (msg->Text.ToString() == "Six Seven") return;
        info("[Chat] {}: {}", msg->Sender.ToString(), msg->Text.ToString());
    }

    void ProxyModHook(UObject*, UFunction*, void*)
    {
        SDK::UClass* ProxyMod = BasicFilesImpleUtils::FindClassByName("ProxyMod_C", false);
        if (!ProxyMod || !Kismet::IsValidClass(ProxyMod)) { error("[ProxyMod] Class not found."); return; }
        SDK::UFunction* InitFunc = ProxyMod->GetFunction("ProxyMod_C", "Init");
        if (!InitFunc || !Kismet::IsValid(InitFunc)) { error("[ProxyMod] Function 'Init' missing."); return; }
        auto* ProxyActor = GetActorOfClass<AActor>(ProxyMod);
        if (!ProxyActor || !Kismet::IsValid(ProxyActor)) { error("[ProxyMod] No active actor found."); return; }
        info("[ProxyMod] Actor '{}' found - executing Init.", GetDisplayName(ProxyActor));
        ProxyActor->ProcessEvent(InitFunc, nullptr);
    }
}

// =========================================================================
// Command handlers
// =========================================================================
namespace Commands
{
    void CmdSet(const ctx& ctx) {
        using namespace VarSystem;
        Cmd_Set(ctx);
    }
    void CmdGet(const ctx& ctx) {
        using namespace VarSystem;
        Cmd_Get(ctx);
    }
    void CmdVars(const ctx& ctx) {
        using namespace VarSystem;
        Cmd_Vars(ctx);
    }
    void CmdUnset(const ctx& ctx) {
        using namespace VarSystem;
        Cmd_Unset(ctx);
    }

    void MakeJSON(const CommandContext& JS)
    {}

    void LogProcessEvents(const CommandContext&)
    {
        if (State::LogAllEventsCallback == 0)
        {
            State::LogAllEventsCallback = GameHooks::OnProcessEventAll(Callbacks::LogAllProcessEvents);
            info("[cmd:logallevents] Logging ALL ProcessEvent calls");
        }
        else
        {
            GameHooks::RemoveHook(State::LogAllEventsCallback);
            State::LogAllEventsCallback = 0;
            info("[cmd:logallevents] ProcessEvent logging disabled");
        }
    }

    //void tpbars(const ctx&){
    //    if (!IsValidOf<APlayerCharacter>(GetLocalPlayer())) return;
    //    std::vector<AActor*> Relevant{};
    //    TArray<AActor*> Barnacles = GetAllActorsOfClass<ABhaBarnacle>();
    //    TArray<AActor*> Apocas = GetAllActorsOfClass<>
    //    TArray<AActor*> Teleports = GetAllActorsOfClass<AReplicatedActor_C>();
    //    for(auto T : Teleports) {
    //        if(!IsValidOf<AReplicatedActor_C>(T)) continue;
    //        if(T->GetOwner() != GetLocalPlayer()) continue;
    //        auto TP = CastChecked<AReplicatedActor_C>(T);
    //        for(auto A : FoundActors) {
    //            if(!IsValidOf<ABhaBarnacle>(A)) continue;
    //            TP->Server_TeleportPlayer()
    //
    //        }
    //    }
    //}
    //void Resup(const ctx&) {
    //    if (!IsValidOf<APlayerCharacter>(GetLocalPlayer())) return;
    //    for (auto i : GetLocalPlayer()->InventoryComponent->GetAllItems()) {
    //        if()
    //    }
    //}

    void ClearTwerkers(const ctx&) {
        if (!CanChangeWorld()) return;
        GetLocalPlayer()->Server_CheatDestroyAllVanityCharacters();
    }

    void SpawnDancer(const ctx& ctx) {
        if (!CanChangeWorld()) return;
        GetLocalPlayer()->Server_CheatDancingCharacterOnSelf(std::strtol(ctx.Arg(1).c_str(), nullptr, 10));
    }

    void ScanDamageMeeterMod(const ctx& = State::dummyCtx) {
        if (!IsValidOf<APlayerCharacter>(GetLocalPlayer())) return;
        TArray<AActor*> FoundActors = GetAllActorsOfClass<AActor>();

        std::vector<AActor*> Filter;
        Filter.reserve(FoundActors.Num());

        for (auto A : FoundActors) {
            if (!IsValidOf<AActor>(A)) continue;
            if (!A->bReplicates) continue;
            if (NoneOf<AActor*>(A->GetOwner(), GetLocalPlayer(), GetLocalPlayer()->GetOwner(),
                GetLocalPlayer()->GetPlayerController(), GetLocalPlayer()->GetPlayerState())) continue;

            Filter.push_back(A);
        }

        std::erase_if(Filter, [](AActor* A) {
            if (!IsValidOf<AActor>(A)) return true;
            if (!A->Class) return true;

            auto ClassName = A->Class->Name.ToString();
            return !ClassName.contains("_DamageList_");
            });
        info("Found {} potential damage meter actors.", Filter.size());
        for (auto A : Filter) {
            info("Potential actor: {}, class: {}", A->Name.ToString(), A->Class->Name.ToString());
        }
    }

    static void Untwerk(APlayerCharacter* Player)
    {
        if (!Player || !Kismet::IsValid(Player)) return;
        if (Player->StaticClass() != SDK::APlayerCharacter::StaticClass()) return;
        if (Player->danceMove != 18) return;
        Player->Server_SetIsDancing(0, -1);
    }

    void Crash(const CommandContext& = State::dummyCtx)
    {
        auto* Local = GetLocalPlayer();
        if (!Local || !Kismet::IsValid(Local)) return info("Aborting crash, no local player");
        if (Kismet::IsServer(Local)) return info("Aborting crash, we are host");
        Local->Server_SpawnEnemies(UEnemyDescriptor::GetDefaultObj(), 100000);
        info("Crash triggered.");
    }
    void Say(const CommandContext& ctx)
    {
        UFSDGameInstance* Instance = GameLib::GetFSDGameInstance(GetWorld());
        if (!IsValidOf<UFSDGameInstance>(Instance)) return;
        AFSDPlayerState* LocalState = Instance->GetLocalFSDPlayerController()->GetFSDPlayerState();
        if (!IsValidOf<AFSDPlayerState>(LocalState)) return;
        const std::string& SenderName = ctx.Arg(1);
        AFSDPlayerState* Sender = nullptr;
        for (AFSDPlayerState* Player : UGameFunctionLibrary::GetFSDGameState(GetWorld())->GetNetworkSortedPlayerArray()) {
            if (!IsValidOf<AFSDPlayerState>(Player)) continue;
            if (Player->GetPlayerName().ToString() == SenderName) {
                Sender = Player;
                break;
            }
        }
        // If no player matched, arg 1 is part of the message, not a name
        const auto msgBegin = IsValidOf<AFSDPlayerState>(Sender)
            ? ctx.args.begin() + 2
            : ctx.args.begin() + 1;

        if (!IsValidOf<AFSDPlayerState>(Sender)) Sender = LocalState;
        std::string msg;

        for (auto it = msgBegin; it != ctx.args.end(); ++it) { if (it != msgBegin) msg += ' '; msg += *it; }

        LocalState->GetPlayerController()->Server_NewMessage(
            Sender->GetPlayerName(),
            FString(ToWide(msg).c_str()),
            Sender->GetChatSenderType()
        );
    }

    void CoilCarve(const CommandContext& ctx)
    {
        auto* L = GetLocalPlayer();
        if (!L || !Kismet::IsValid(L)) return;
        auto* WPN = L->InventoryComponent->GetItem(EItemCategory::SecondaryWeapon);
        if (!WPN || !Kismet::IsValid(WPN) || !WPN->IsA(ACoilGun::StaticClass())) return;
        auto* CoilGun = CastChecked<ACoilGun>(WPN);
        FVector_NetQuantize Start{}, End{};
        Start.X = SafeStof(ctx.Arg(1)); Start.Y = SafeStof(ctx.Arg(2)); Start.Z = SafeStof(ctx.Arg(3));
        End.X = SafeStof(ctx.Arg(4)); End.Y = SafeStof(ctx.Arg(5)); End.Z = SafeStof(ctx.Arg(6));
        CoilGun->Server_HitTerrain(Start, End, SafeStof(ctx.Arg(7)));
    }

    void WPInfo(const CommandContext& = State::dummyCtx)
    {
        using namespace ObjectCast;

        if (IsOnSpaceRig()) { spdlog::info("[wpinfo] on space rig"); return; }

        std::vector<AFSDPawn*> Enemies = GetAliveNonFriendlies();
        if (Enemies.empty()) { spdlog::info("[wpinfo] no enemies"); return; }

        APlayerCharacter* LocalPlayer = GetLocalPlayer();
        FVector CamLoc = {};
        if (IsValidOf<APlayerCharacter>(LocalPlayer)) {
            if (auto* Ctrl = Cast<AFSDPlayerController>(LocalPlayer->Controller))
                if (auto* CamMgr = Ctrl->PlayerCameraManager)
                    CamLoc = CamMgr->GetCameraLocation();
        }

        AFSDPawn* Target = nullptr;
        float BestDist = FLT_MAX;
        for (AFSDPawn* E : Enemies) {
            if (!IsValidOf<AFSDPawn>(E)) continue;
            FVector D = E->K2_GetActorLocation() - CamLoc;
            float Dist = D.X*D.X + D.Y*D.Y + D.Z*D.Z;
            if (Dist < BestDist) { BestDist = Dist; Target = E; }
        }
        if (!Target) { spdlog::info("[wpinfo] no valid target"); return; }

        const char* Branch = Cast<AEnemyDeepPathfinderCharacter>(Target) ? "DeepPathfinder"
                           : Cast<AEnemyPawn>(Target)                  ? "EnemyPawn"
                           : "unknown";
        auto Meshes = Helpers::GetEnemyMeshes(Target);
        spdlog::info("[wpinfo] branch={} meshes={}", Branch, Meshes.size());

        int32 wpCount = 0;
        for (int32 m = 0; m < (int32)Meshes.size(); ++m)
        {
            auto* Mesh = Meshes[m];
            auto* SkelAsset = Mesh->SkeletalMesh;
            auto* PhysAsset = SkelAsset ? SkelAsset->PhysicsAsset : nullptr;
            spdlog::info("[wpinfo] mesh[{}] bones={} skelMesh={} physAsset={} bodies={}",
                m, Mesh->GetNumBones(),
                SkelAsset ? "ok" : "NULL", PhysAsset ? "ok" : "NULL",
                PhysAsset ? PhysAsset->SkeletalBodySetups.Num() : 0);
            if (!PhysAsset) continue;

            for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); ++i) {
                auto* Body = PhysAsset->SkeletalBodySetups[i];
                if (!Body) continue;
                auto* FSDMat = Body->PhysMaterial ? Cast<UFSDPhysicalMaterial>(Body->PhysMaterial) : nullptr;
                if (!FSDMat || !FSDMat->IsWeakPoint) continue;

                ++wpCount;
                const int32   BoneIdx = Mesh->GetBoneIndex(Body->BoneName);
                const FVector Pos     = Mesh->GetSocketLocation(Body->BoneName);
                const bool    bVis    = IsValidOf<APlayerCharacter>(LocalPlayer)
                    ? Helpers::IsWeakpointVisible(LocalPlayer, Target, CamLoc, Pos, Body->BoneName)
                    : false;
                const auto WpState = Helpers::GetBreakableWpState(Target, Mesh, Body->BoneName, FSDMat);
                if (WpState.isBreakable)
                {
                    if (WpState.health >= 0.f)
                        spdlog::info("[wpinfo]  WP[{}] bone='{}' boneIdx={} mult={:.2f} {} breakable hp={:.1f}{}",
                            i, Body->BoneName.ToString(), BoneIdx, FSDMat->DamageMultiplier,
                            bVis ? "VIS" : "OCC", WpState.health,
                            WpState.isDestroyed ? " DESTROYED" : "");
                    else
                        spdlog::info("[wpinfo]  WP[{}] bone='{}' boneIdx={} mult={:.2f} {} breakable{}",
                            i, Body->BoneName.ToString(), BoneIdx, FSDMat->DamageMultiplier,
                            bVis ? "VIS" : "OCC",
                            WpState.isDestroyed ? " DESTROYED" : "");
                }
                else
                {
                    spdlog::info("[wpinfo]  WP[{}] bone='{}' boneIdx={} mult={:.2f} {} pos=({:.0f},{:.0f},{:.0f})",
                        i, Body->BoneName.ToString(), BoneIdx, FSDMat->DamageMultiplier,
                        bVis ? "VIS" : "OCC",
                        Pos.X, Pos.Y, Pos.Z);
                }
            }
        }
        spdlog::info("[wpinfo] {} weakpoint(s) total", wpCount);
    }

    void FearAll(const CommandContext& = State::dummyCtx)
    {
        APlayerCharacter* Player = GetLocalPlayer();
        if (!Kismet::IsValid(Player)) return;

        ACoilGun* CoilGun = Cast<ACoilGun>(Player->InventoryComponent->GetItem(EItemCategory::SecondaryWeapon));
        if (!Kismet::IsValid(CoilGun)) return;

        TArray<AActor*> FoundActors;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AFSDPawn::StaticClass(), &FoundActors);

        if (FoundActors.Num() == 0) return;

        const bool bIsServer = Kismet::IsServer(GetWorld());

        if (bIsServer)
        {
            for (AActor* Actor : FoundActors)
            {
                if (Kismet::IsValid(Actor))
                {
                    CoilGun->Server_FearTarget(Actor);
                }
            }
        }
        else
        {
            // Convert to a clean vector for the throttled queue
            std::vector<AFSDPawn*> Pawns;
            Pawns.reserve(FoundActors.Num());

            for (AActor* Actor : FoundActors)
            {
                if (AFSDPawn* Pawn = Cast<AFSDPawn>(Actor))
                {
                    Pawns.push_back(Pawn);
                }
            }

            EnqueueThrottled<AFSDPawn*>(std::move(Pawns), std::chrono::milliseconds(16),
                [CoilGun](AFSDPawn* Target, size_t Index, size_t Total) -> bool
                {
                    if (Kismet::IsValid(CoilGun) && Kismet::IsValid(Target))
                    {
                        CoilGun->Server_FearTarget(Target);
                    }
                    return true;
                });
        }
    }

    void LogChat(const CommandContext& = State::dummyCtx)
    {
        if (State::ServerChatCallback == 0)
        {
            //State::ClientChatCallback = GameHooks::OnProcessEventByNameAndClass(
            //    "Server_NewMessage", SDK::APlayerController::StaticClass(), Callbacks::MessageIntercept);
            //if(!Kismet::IsServer(nullptr))
            State::ServerChatCallback = GameHooks::OnProcessEventByNameAndClass(
                "ClientNewMessage", SDK::AFSDGameState::StaticClass(), Callbacks::ServerMessageIntercept);
            info("[cmd:logchat] Chat logging enabled");
        }
        else
        {
            //GameHooks::RemoveHook(State::ClientChatCallback);
            //if (!Kismet::IsServer(nullptr))
            GameHooks::RemoveHook(State::ServerChatCallback);
            /* State::ClientChatCallback = */State::ServerChatCallback = 0;
            info("[cmd:logchat] Chat logging disabled");
        }
    }

    void SixSeven(const CommandContext& = State::dummyCtx)
    {
        auto  Players = ActorLib::GetAllPlayerCharacters(GetWorld());
        auto* Player = GetLocalPlayer();
        if (!Kismet::IsValid(Player)) return;
        for (auto* Curr : Players)
        {
            if (Curr == Player || Curr->danceMove != 18) continue;
            Player->GetPlayerController()->Server_NewMessage(
                Curr->PlayerState->GetPlayerName(), FString(L"Six Seven"),
                Curr->GetPlayerState()->GetChatSenderType());
        }
    }

    void Dance(const CommandContext& = State::dummyCtx)
    {
        auto* Player = GetLocalPlayer();
        if (!Kismet::IsValid(Player)) { warn("[cmd:dance] Local player not valid."); return; }
        info("[cmd:dance] Player: '{}', danceMove: {}",
            Player->PlayerState->GetPlayerName().ToString(), Player->danceMove);
    }

    void Rename(const CommandContext& ctx)
    {
        auto* Player = GetLocalPlayer();
        if (!Kismet::IsValid(Player)) { warn("[cmd:rename] Local player not valid."); return; }
        info("[cmd:rename] Current name: '{}'", ObjToStr(Player));
        std::string newName;
        for (size_t i = 1; i < ctx.args.size(); ++i) { if (i > 1) newName += ' '; newName += ctx.args[i]; }
        UObjectVCalls::Rename::Call(Player, ToWide(newName).c_str(), Player->Outer, REN::None);
        info("[cmd:rename] New name: '{}'", ObjToStr(Player));
    }

    void ReadName(const CommandContext& = State::dummyCtx)
    {
        auto* Player = GetLocalPlayer();
        if (!Kismet::IsValid(Player)) { warn("[cmd:readname] Local player not valid."); return; }
        info("[cmd:readname] Player name: '{}'", ObjToStr(Player));
    }

    void Twerk(const CommandContext& = State::dummyCtx)
    {
        if (State::DanceCallback == 0)
        {
            auto* DanceFunc = SDK::APlayerCharacter::StaticClass()
                ->GetFunction("PlayerCharacter", "Server_SetIsDancing");
            State::DanceCallback = GameHooks::OnProcessEventAdvanced(
                Callbacks::DanceIntercept, "Server_SetIsDancing",
                SDK::APlayerCharacter::StaticClass(), nullptr, DanceFunc);
            info("[cmd:twerk] Anti-twerk enabled");
        }
        else
        {
            GameHooks::RemoveHook(State::DanceCallback);
            State::DanceCallback = 0;
            info("[cmd:twerk] Anti-twerk disabled");
        }
    }

    void DoUntwerk(const CommandContext& = State::dummyCtx)
    {
        auto  Players = ActorLib::GetAllPlayerCharacters(GetWorld());
        auto* Local = GetLocalPlayer();
        for (auto* player : Players) if (player != Local) Untwerk(player);
    }

    void FindClass(const CommandContext& ctx)
    {
        const std::string needle = ctx.Arg(1);
        for (int i = 0; i < UObject::GObjects->Num(); ++i)
        {
            auto* obj = UObject::GObjects->GetByIndex(i);
            if (!obj || !obj->IsA(UClass::StaticClass())) continue;
            std::string n = obj->GetName();
            if (PropertyInspector::NameMatches(n, needle, true))
                info("  [class] {} -> CDO: {}", n,
                    static_cast<UClass*>(obj)->ClassDefaultObject
                    ? static_cast<UClass*>(obj)->ClassDefaultObject->GetName() : "null");
        }
    }

    void Inventory(const CommandContext& = State::dummyCtx)
    {
        auto* Player = GetLocalPlayer();
        if (!Kismet::IsValid(Player)) { warn("[cmd:inventory] Local player not valid."); return; }
        auto* Inventory = Player->InventoryComponent;
        if (!Kismet::IsValid(Inventory)) { warn("[cmd:inventory] Inventory not valid."); return; }
        auto* ItemBaseClass = SDK::AAmmoDrivenWeapon::StaticClass();
        for (auto* Item : Inventory->GetAllItems())
        {
            if (Kismet::IsValid(Item)) DumpItemProperties<true>(Item, ItemBaseClass);
            else warn("[cmd:inventory] Skipping invalid item.");
        }
    }

    void IgnoreProxy(const CommandContext& = State::dummyCtx)
    {
        if (State::ProxyModCallback == 0)
        {
            SDK::UClass* ProxyMod = BasicFilesImpleUtils::FindClassByName("ProxyMod_C", false);
            if (!ProxyMod || !Kismet::IsValidClass(ProxyMod))
            {
                error("[cmd:ignoreproxy] Failed to find class 'ProxyMod_C'."); return;
            }
            State::ProxyModCallback = GameHooks::OnProcessEventByNameAndClass(
                "Init", ProxyMod, Callbacks::ProxyModHook);
            info("[cmd:ignoreproxy] ProxyMod hook enabled");
        }
        else
        {
            GameHooks::RemoveHook(State::ProxyModCallback);
            State::ProxyModCallback = 0;
            info("[cmd:ignoreproxy] ProxyMod hook disabled");
        }
    }

    void Prop(const CommandContext& ctx) { PropertyInspector::DispatchCommand(ctx); }

    void Exec(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 2) { warn("[cmd:exec] Usage: exec <command>"); return; }
        std::string cmd;
        for (size_t i = 1; i < ctx.args.size(); ++i) { if (i > 1) cmd += ' '; cmd += ctx.args[i]; }
        ::Exec(cmd);
    }

    void StopTick(const CommandContext& = State::dummyCtx)
    {
        if (State::TickCallback == 0)
        {
            State::TickCallback = GameHooks::OnProcessEventByNameAndClass(
                "ReceiveTick", SDK::AActor::StaticClass(), Callbacks::TickListener);
            info("[cmd:stoptick] tick listener enabled");
        }
        else
        {
            GameHooks::RemoveHook(State::TickCallback);
            State::TickCallback = 0;
            info("[cmd:stoptick] tick listener disabled");
        }
    }

    void BeginPlay(const CommandContext& = State::dummyCtx)
    {
        if (State::BeginPlayCallback == 0)
        {
            State::BeginPlayCallback = GameHooks::OnProcessEventByNameAndClass(
                "ReceiveBeginPlay", SDK::AActor::StaticClass(),
                [](UObject* object, UFunction*, void*)
                {
                    if (!object || !object->Class) return;
                    for (const wchar_t* name : { L"Core_C", L"NoVines_C", L"TickManager_C" })
                        if (IsChildOfByName(object, name))
                        {
                            info("[BeginPlay] {} spawned: {}",
                                object->Class->Name.ToString(), object->Name.ToString());
                            break;
                        }
                });
            info("[beginplay] Watcher registered");
        }
        else
        {
            GameHooks::RemoveHook(State::BeginPlayCallback);
            State::BeginPlayCallback = 0;
            info("[beginplay] disabled");
        }
    }

    void ScanAllClasses(const CommandContext& = State::dummyCtx)
    {
        int totalClasses = 0, totalFuncs = 0;
        info("[cmd:scanall] Starting global CDO scan...");
        for (int i = 0; i < UObject::GObjects->Num(); ++i)
        {
            UObject* Obj = UObject::GObjects->GetByIndex(i);
            if (!Obj || !Obj->IsA(UClass::StaticClass())) continue;
            UClass* Class = static_cast<UClass*>(Obj);
            UObject* CDO = Class->ClassDefaultObject;
            if (!CDO) continue;
            std::vector<std::string> classFuncs;
            for (auto* field : UFieldRange(Class->Children))
            {
                if (!field->IsA(UFunction::StaticClass())) continue;
                auto* Func = static_cast<UFunction*>(field);
                auto  flags = static_cast<EFunctionFlags>(Func->FunctionFlags);
                if (!(flags & EFunctionFlags::Net))       continue;
                if (!(flags & EFunctionFlags::NetServer)) continue;
                classFuncs.push_back(Scan::BuildFuncSig(Func));
            }
            if (classFuncs.empty()) continue;
            std::sort(classFuncs.begin(), classFuncs.end());
            classFuncs.erase(std::unique(classFuncs.begin(), classFuncs.end()), classFuncs.end());
            ++totalClasses;
            info("╔══ Class: {}", Class->GetName());
            info("╠═ [CDO] {} server RPCs found", classFuncs.size());
            for (size_t j = 0; j < classFuncs.size(); ++j)
            {
                info("║   {} {}", j == classFuncs.size() - 1 ? "└──" : "├──", classFuncs[j]);
                ++totalFuncs;
            }
            info("╚══════════════════════════");
        }
        info("[scanall] Done — Scanned {} classes, found {} unique server RPCs", totalClasses, totalFuncs);
    }

    void ScanReplicated(const CommandContext& ctx)
    {
        APlayerCharacter* Local = GetLocalPlayer();
        if (!Local) { warn("[cmd:scan] No local player."); return; }
        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass(), &AllActors);
        int totalActors = 0, totalFuncs = 0;
        ScanResponse sr{};

        auto WriteToSr = [&](const std::string& sig, const std::string& owner)
            {
                if (sr.count >= MAX_SCAN_RESULT) return;
                auto& out = sr.funcs[sr.count++];
                auto paren = sig.find('(');
                std::string fname = paren != std::string::npos ? sig.substr(0, paren) : sig;
                std::string params;
                if (paren != std::string::npos)
                {
                    params = sig.substr(paren + 1);
                    auto close = params.rfind(')');
                    if (close != std::string::npos) params = params.substr(0, close);
                    for (auto& c : params) if (c == '\n' || c == '\t' || c == '║' || c == ' ') c = ' ';
                    auto new_end = std::unique(params.begin(), params.end(),
                        [](char a, char b) { return a == ' ' && b == ' '; });
                    params.erase(new_end, params.end());
                    if (!params.empty() && params.front() == ' ') params = params.substr(1);
                    if (!params.empty() && params.back() == ' ') params.pop_back();
                }
                strncpy_s(out.name, fname.c_str(), MAX_FUNC_NAME - 1);
                strncpy_s(out.owner, owner.c_str(), MAX_FUNC_NAME - 1);
                strncpy_s(out.params, params.c_str(), MAX_PARAM_STR - 1);
            };

        for (auto* Actor : AllActors)
        {
            if (!Actor || !Kismet::IsValid(Actor) || !Actor->bReplicates) continue;
            if (NoneOf<AActor*>(Actor->GetOwner(), Local, Local->GetOwner(),
                Local->GetPlayerController(), Local->GetPlayerState())) continue;
            std::vector<std::string> actorFuncs;
            Scan::ScanFunctions(Actor, actorFuncs);
            TArray<UActorComponent*> Components =
                Actor->K2_GetComponentsByClass(UActorComponent::StaticClass());
            std::vector<std::pair<std::string, std::vector<std::string>>> compResults;
            for (auto* Comp : Components)
            {
                if (!Comp || !Kismet::IsValid(Comp)) continue;
                std::vector<std::string> compFuncs;
                Scan::ScanFunctions(Comp, compFuncs);
                if (!compFuncs.empty()) compResults.emplace_back(Comp->Class->GetName(), std::move(compFuncs));
            }
            if (actorFuncs.empty() && compResults.empty()) continue;
            ++totalActors;
            info("╔══ {} ({})", Actor->GetName(), Actor->Class->GetName());
            if (!actorFuncs.empty())
            {
                info("╠═ [actor] {} server RPCs", actorFuncs.size());
                for (size_t i = 0; i < actorFuncs.size(); ++i)
                {
                    bool last = (i == actorFuncs.size() - 1) && compResults.empty();
                    info("║   {} {}", last ? "└──" : "├──", actorFuncs[i]);
                    WriteToSr(actorFuncs[i], Actor->Class->GetName());
                    ++totalFuncs;
                }
            }
            for (size_t ci = 0; ci < compResults.size(); ++ci)
            {
                const auto& [compClass, funcs] = compResults[ci];
                bool lastComp = (ci == compResults.size() - 1);
                info("╠═ [component] {}", compClass);
                for (size_t i = 0; i < funcs.size(); ++i)
                {
                    bool last = lastComp && (i == funcs.size() - 1);
                    info("║   {} {}", last ? "└──" : "├──", funcs[i]);
                    WriteToSr(funcs[i], compClass);
                    ++totalFuncs;
                }
            }
            info("╚══════════════════════════");
        }
        info("[scan] Done — {} actors, {} server RPCs", totalActors, totalFuncs);
        if (g_pRespBuffer && !g_pRespBuffer->ready.load(std::memory_order_acquire))
        {
            g_pRespBuffer->type = ResponseType::Scan;
            memcpy(&g_pRespBuffer->data.scan, &sr, sizeof(sr));
            g_pRespBuffer->seq.store(ctx.seq, std::memory_order_release);
            g_pRespBuffer->ready.store(true, std::memory_order_release);
            SetEvent(g_hRespEvent);
        }
    }

    void Call(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 2) { warn("[cmd:call] Usage: call <FunctionName> [arg0 arg1 ...]"); return; }

        std::vector<std::string> rest{ ctx.args.begin() + 1, ctx.args.end() };
        std::string funcName; std::vector<std::string> rawArgs;

        auto sep = std::find(rest.begin(), rest.end(), "::");
        if (sep == rest.end()) { funcName = rest[0]; rawArgs = { rest.begin() + 1, rest.end() }; }
        else
        {
            for (auto it = rest.begin(); it != sep; ++it) { if (!funcName.empty()) funcName += ' '; funcName += *it; }
            rawArgs = { sep + 1, rest.end() };
        }
        if (!State::ScannedFunctions.contains(funcName) &&
            !State::ScannedFunctionVariantsByName.contains(funcName))
        {
            info("[cmd:call] '{}' not cached, rescanning...", funcName); Scan::DoScan();
        }

        auto PrintVariants = [&](const std::string& baseName)
            {
                auto vit = State::ScannedFunctionVariantsByName.find(baseName);
                if (vit == State::ScannedFunctionVariantsByName.end() || vit->second.empty()) return;
                info("[cmd:call] '{}' is ambiguous; use one of:", baseName);
                for (size_t i = 0; i < vit->second.size(); i++)
                    info("[cmd:call]   [{}] {}", i, vit->second[i]);
            };

        auto ResolveCachedCallTarget = [&]() -> State::ScannedFunction*
            {
                auto ResolveUniqueVariant = [&](const std::string& key) -> State::ScannedFunction*
                    {
                        auto it = State::ScannedFunctions.find(key);
                        return it == State::ScannedFunctions.end() ? nullptr : &it->second;
                    };

                auto ResolveByBaseName = [&](const std::string& baseName) -> State::ScannedFunction*
                    {
                        auto vit = State::ScannedFunctionVariantsByName.find(baseName);
                        if (vit == State::ScannedFunctionVariantsByName.end() || vit->second.empty()) return nullptr;
                        if (vit->second.size() != 1)
                        {
                            PrintVariants(baseName);
                            return nullptr;
                        }

                        return ResolveUniqueVariant(vit->second[0]);
                    };

                State::ScannedFunction* candidate = ResolveUniqueVariant(funcName);
                if (!candidate)
                    candidate = ResolveByBaseName(funcName);
                if (!candidate) return nullptr;

                auto& [Func, Owner, FunctionName, OwnerName, OwnerClassName, ExplicitName] = *candidate;
                if (!Owner || !IsValidRaw(Owner) || !IsValid(Owner) || !IsInActiveWorld(Owner))
                {
                    const std::string explicitName = ExplicitName;
                    const std::string functionName = FunctionName;
                    warn("[cmd:call] Owner stale or from old world for '{}'; rescanning.", funcName);
                    Scan::DoScan();
                    candidate = ResolveUniqueVariant(explicitName);
                    if (!candidate)
                        candidate = ResolveByBaseName(functionName);
                    if (!candidate) return nullptr;
                }

                if (!IsValidOf<UFunction>(candidate->Func))
                {
                    const std::string explicitName = candidate->ExplicitName;
                    const std::string functionName = candidate->FunctionName;
                    warn("[cmd:call] UFunction stale for '{}'; rescanning.", funcName);
                    Scan::DoScan();
                    candidate = ResolveUniqueVariant(explicitName);
                    if (!candidate)
                        candidate = ResolveByBaseName(functionName);
                    if (!candidate) return nullptr;
                }

                return candidate;
            };

        State::ScannedFunction* target = ResolveCachedCallTarget();
        if (!target) { warn("[cmd:call] '{}' not found.", funcName); return; }

        auto* Func = target->Func;
        auto* Owner = target->Owner;
        std::vector<FProperty*> parmProps;
        for (FField* field : FFieldRange(Func->ChildProperties))
        {
            if (!FieldCast::IsA<FProperty>(field)) continue;
            FProperty* Prop = static_cast<FProperty*>(field);
            EPropertyFlags  pf = static_cast<EPropertyFlags>(Prop->PropertyFlags);
            if (!(pf & EPropertyFlags::Parm))    continue;
            if (pf & EPropertyFlags::ReturnParm) continue;
            parmProps.push_back(Prop);
        }
        const int32 parmsSize = PropertyInspector::ComputeParmsSize(Func);
        std::vector<uint8> parmsBuf(parmsSize, 0);
        if (rawArgs.size() > parmProps.size())
            warn("[cmd:call] {} args given but '{}' only has {} param(s); extras ignored.",
                rawArgs.size(), funcName, parmProps.size());
        size_t filled = std::min(rawArgs.size(), parmProps.size());
        for (size_t i = 0; i < filled; ++i)
        {
            ExpandResult expanded = Expand(rawArgs[i]);
            if (!expanded.isValid) continue;
            uintptr_t base = reinterpret_cast<uintptr_t>(parmsBuf.data());
            if (expanded.object)
            {
                FieldCast::Visit(parmProps[i], [&]<typename T>(T * p)
                {
                    if constexpr (std::is_same_v<T, FObjectProperty> ||
                        std::is_same_v<T, FObjectPropertyBase> ||
                        std::is_same_v<T, FClassProperty> ||
                        std::is_same_v<T, FWeakObjectProperty>)
                        *GetPropertyPtr<UObject*>(base, p->Offset) = expanded.object;
                    else
                    {
                        warn("[cmd:call]   [{}] param '{}' not object property — name fallback",
                            i, p->Name.ToString());
                        PropertyInspector::WriteParam(parmProps[i], expanded.object->GetName(), parmsBuf.data());
                    }
                });
            }
            else PropertyInspector::WriteParam(parmProps[i], expanded.token, parmsBuf.data());
        }
        if (!parmProps.empty())
        {
            info("[cmd:call] '{}' ({} param(s), {} supplied):", funcName, parmProps.size(), rawArgs.size());
            for (size_t i = 0; i < parmProps.size(); ++i)
            {
                std::string display;
                if (i >= rawArgs.size()) display = "<zeroed>";
                else
                {
                    ExpandResult expanded = Expand(rawArgs[i]);
                    if (rawArgs[i] == expanded.token) display = rawArgs[i];
                    else if (expanded.object)         display = rawArgs[i] + " → [object] " + expanded.object->GetName();
                    else                              display = rawArgs[i] + " → " + expanded.token;
                }
                info("[cmd:call]   [{}] {} = {}", i, parmProps[i]->Name.ToString(), display);
            }
        }
        uint32 savedFlags = Func->FunctionFlags;
        if (static_cast<EFunctionFlags>(Func->FunctionFlags) & EFunctionFlags::Native) Func->FunctionFlags |= 0x400;
        Owner->ProcessEvent(Func, parmsSize > 0 ? parmsBuf.data() : nullptr);
        Func->FunctionFlags = savedFlags;
        for (FProperty* Prop : parmProps)
            FieldCast::Visit(Prop, [&]<typename T>(T * p)
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(parmsBuf.data());
            if constexpr (std::is_same_v<T, FStrProperty>)
                GetPropertyPtr<FString>(base, p->Offset)->~FString();
            else if constexpr (std::is_same_v<T, FNameProperty>)
                GetPropertyPtr<FName>(base, p->Offset)->~FName();
        });
        info("[cmd:call] Called '{}' on '{}'.", funcName, Owner->GetName());
    }

    void TeleportAllToSelf(const ctx& = State::dummyCtx) {
        if (!IsValidOf<APlayerCharacter>(GetLocalPlayer())) return;
        TArray<APlayerCharacter*> Players = ActorLib::GetAllPlayerCharacters(GetWorld());
        FVector Coords = GetLocalPlayer()->K2_GetActorLocation();
        for (auto TargetPlayer : Players)
            if (Kismet::IsServer(GetWorld()))
            {
                FHitResult Hit;
                if (IsValidOf<APlayerCharacter>(TargetPlayer))
                    TargetPlayer->K2_SetActorLocation(Coords, false, &Hit, true);
            }
            else
            {
                for (auto* Actor : GetAllActorsOfClass<AReplicatedActor_C>())
                    static_cast<AReplicatedActor_C*>(Actor)->Server_TeleportPlayer(TargetPlayer, Coords);
            }
    }

    void PlayerTeleport(const ctx& ctx) {
        if (!IsValidOf<APlayerCharacter>(GetLocalPlayer())) return;

        TArray<APlayerCharacter*> Players = ActorLib::GetAllPlayerCharacters(GetWorld());
        if (Players.Num() == 0) return;

        MutableContext mutableCtx = MutableContext(ctx);
        FVector Coords{};
        std::vector<std::pair<std::string, APlayerCharacter*>> NameToPlayer{};
        bool    foundDestinationPlayer = false;
        bool    targetIsSelf = false;
        bool    foundPlayerToMove = false;
        NameToPlayer.reserve(Players.Num());

        for (APlayerCharacter* Player : Players) {
            if (!IsValidOf<APlayerCharacter>(Player)) continue;
            std::pair<std::string, APlayerCharacter*> entry{ Player->PlayerState->GetPlayerName().ToString(), Player };
            NameToPlayer.push_back(entry);
        }

        std::erase(mutableCtx.args, "ptp");

        APlayerCharacter* TargetPlayer = nullptr;
        APlayerCharacter* DestPlayer = nullptr;

        // --- Parse "target <name>" ---
        if (auto it = std::find(mutableCtx.args.begin(), mutableCtx.args.end(), "target"); it != mutableCtx.args.end()) {
            if (++it == mutableCtx.args.end()) { warn("[cmd:ptp] 'target' specified but no name given."); return; }

            std::string searchString;
            while (it != mutableCtx.args.end() && *it != "dest") {
                if (!searchString.empty()) searchString += " ";
                searchString += *it;
                ++it;
            }

            for (auto& [name, player] : NameToPlayer) {
                if (name.find(searchString) != std::string::npos) {
                    TargetPlayer = player;
                    foundPlayerToMove = true;

                    if (player == GetLocalPlayer()) targetIsSelf = true;
                    break;
                }
            }
            if (!foundPlayerToMove) { warn("[cmd:ptp] Target player '{}' not found.", searchString); return; }
        }

        // --- Parse "dest <name>" ---
        if (auto it = std::find(mutableCtx.args.begin(), mutableCtx.args.end(), "dest"); it != mutableCtx.args.end()) {
            if (++it == mutableCtx.args.end()) { warn("[cmd:ptp] 'dest' specified but no name given."); return; }

            std::string searchString;
            while (it != mutableCtx.args.end() && *it != "target") {
                if (!searchString.empty()) searchString += " ";
                searchString += *it;
                ++it;
            }

            for (auto& [name, player] : NameToPlayer) {
                if (name.find(searchString) != std::string::npos) {
                    DestPlayer = player;
                    foundDestinationPlayer = true;
                    Coords = player->K2_GetActorLocation();
                    break;
                }
            }
            if (!foundDestinationPlayer) { warn("[cmd:ptp] Destination player '{}' not found.", searchString); return; }
        }

        // --- Resolve cases ---
        // Only one positional arg (no target/dest keywords): tp self to that player
        if (!foundPlayerToMove && !foundDestinationPlayer) {
            if (mutableCtx.args.size() == 1) {
                std::string searchString = mutableCtx.args[0];
                for (auto& [name, player] : NameToPlayer) {
                    if (name.find(searchString) != std::string::npos) {
                        TargetPlayer = GetLocalPlayer();
                        DestPlayer = player;
                        Coords = player->K2_GetActorLocation();
                        foundPlayerToMove = true;
                        foundDestinationPlayer = true;
                        targetIsSelf = true;
                        break;
                    }
                }
                if (!foundPlayerToMove) { warn("[cmd:ptp] Player '{}' not found.", searchString); return; }
            }
            else {
                warn("[cmd:ptp] Usage: ptp <name>  |  ptp target <name>  |  ptp dest <name>  |  ptp target <name> dest <name>");
                return;
            }
        }

        // target given, no dest → tp target to self
        if (foundPlayerToMove && !foundDestinationPlayer) {
            Coords = GetLocalPlayer()->K2_GetActorLocation();
        }

        // dest given, no target → tp self to dest
        if (!foundPlayerToMove && foundDestinationPlayer) {
            TargetPlayer = GetLocalPlayer();
            targetIsSelf = true;
        }

        // --- Execute teleport ---
        if (Kismet::IsServer(GetWorld()))
        {
            FHitResult Hit;
            if (IsValidOf<APlayerCharacter>(TargetPlayer))
                TargetPlayer->K2_SetActorLocation(Coords, false, &Hit, true);
        }
        else
        {
            for (auto* Actor : GetAllActorsOfClass<AReplicatedActor_C>())
                static_cast<AReplicatedActor_C*>(Actor)->Server_TeleportPlayer(TargetPlayer, Coords);
        }
    }

    void Teleport(const CommandContext& ctx)
    {
        auto* Local = GetLocalPlayer();
        if (!Kismet::IsValid(Local)) return;
        auto mutableCtx = MutableContext(ctx);
        (void)std::erase(mutableCtx.args, "tp");
        bool sweep = std::erase(mutableCtx.args, "--sweep") > 0;
        bool Relative = std::erase(mutableCtx.args, "--rel") > 0;
        if (mutableCtx.ArgCount() < 3) { warn("[cmd:tp] Usage: tp <x> <y> <z> [--rel] [--sweep]"); return; }
        FVector Dir = { std::strtof(mutableCtx.Arg(0).c_str(), nullptr),
                        std::strtof(mutableCtx.Arg(1).c_str(), nullptr),
                        std::strtof(mutableCtx.Arg(2).c_str(), nullptr) };
        FVector Coords = Dir;
        if (Relative)
        {
            FVector Old = Local->K2_GetActorLocation();
            FVector Forward = Local->GetPlayerController()->GetActorForwardVector() * Dir.X;
            FVector Right = Local->GetPlayerController()->GetActorRightVector() * Dir.Y;
            FVector Up = Local->GetPlayerController()->GetActorUpVector() * Dir.Z;
            Coords = Old + Forward + Right + Up;
        }
        if (Kismet::IsServer(GetWorld()))
        {
            FHitResult Hit; Local->K2_SetActorLocation(Coords, sweep, &Hit, true);
        }
        else
        {
            TArray<AActor*> Actors;
            UGameplayStatics::GetAllActorsOfClass(GetWorld(), AReplicatedActor_C::StaticClass(), &Actors);
            for (auto* Actor : Actors)
                static_cast<AReplicatedActor_C*>(Actor)->Server_TeleportPlayer(Local, Coords);
        }
        info("[cmd:tp] Teleported to ({}, {}, {})", Coords.X, Coords.Y, Coords.Z);
    }

    void HideSound(const CommandContext& = State::dummyCtx)
    {
        if (!IsOnSpaceRig()) return;
        auto* Local = GetLocalPlayer();
        if (!Kismet::IsValid(Local)) return;
        if (Kismet::IsServer(GetWorld()))
        {
            FHitResult Hit; Local->K2_SetActorLocation({ 0, 0, 12550 }, false, &Hit, true);
        }
        else
        {
            TArray<AActor*> Actors;
            UGameplayStatics::GetAllActorsOfClass(GetWorld(), AReplicatedActor_C::StaticClass(), &Actors);
            for (auto* Actor : Actors)
                static_cast<AReplicatedActor_C*>(Actor)->Server_TeleportPlayer(Local, { 0, 0, 12550 });
        }
    }

    void Troll(const CommandContext& = State::dummyCtx)
    {
        APlayerCharacter* Local = GetLocalPlayer();
        AReplicatedActor_C* ActualTeleporter = nullptr;
        TArray<AActor*> AllActors, Teleporters;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass(), &AllActors);
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AReplicatedActor_C::StaticClass(), &Teleporters);
        for (auto* T : Teleporters)
            if (T && Kismet::IsValid(T) && T->GetOwner() == Local)
            {
                ActualTeleporter = static_cast<AReplicatedActor_C*>(T); break;
            }
        if (!ActualTeleporter) { l::warn("[cmd:troll] No teleporter found."); return; }
        std::vector<AActor*> actors;
        for (auto* A : AllActors) if (A && Kismet::IsValid(A) && A->bReplicates) actors.push_back(A);
        info("[cmd:troll] Found {} actors, starting teleport loop.", actors.size());
        auto rng = std::make_shared<std::mt19937>(std::random_device{}());
        auto teleporter = ActualTeleporter;
        EnqueueThrottled<AActor*>(std::move(actors), std::chrono::milliseconds(16),
            [rng, teleporter](AActor* target, size_t i, size_t total) -> bool
            {
                if (!Kismet::IsValid(teleporter)) return false;
                if (target && Kismet::IsValid(target))
                {
                    auto rand = [&](float lo, float hi) {
                        return std::uniform_real_distribution<float>(lo, hi)(*rng); };
                    auto vec = FVector{ rand(-500000,500000), rand(-500000,500000), rand(-500000,500000) };
                    teleporter->Server_TeleportPlayer(target, vec);
                    target->K2_SetActorLocation(vec, false, nullptr, true);
                    info("[cmd:troll] Teleported '{}' ({}/{})", target->GetName(), i + 1, total);
                }
                return true;
            });
    }

    void SpawnEnemies(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 4) { warn("[cmd:spawnenemies] Usage: spawnenemies <descriptor> <count> <x> <y> <z>"); return; }
        FString Descriptor = ToFString(ctx.Arg(1));
        int     Count = std::stoi(ctx.Arg(2));
        FVector Location = { std::strtof(ctx.Arg(3).c_str(), nullptr),
                                std::strtof(ctx.Arg(4).c_str(), nullptr),
                                std::strtof(ctx.Arg(5).c_str(), nullptr) };
        TArray<AActor*> Actors;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AReplicatedActor_C::StaticClass(), &Actors);
        for (auto* Actor : Actors)
            static_cast<AReplicatedActor_C*>(Actor)->Server_SpawnEnemy(Descriptor, Count, Location, false, false);
        info("[cmd:spawnenemies] Spawned {} '{}' at ({}, {}, {})",
            Count, ctx.Arg(1), Location.X, Location.Y, Location.Z);
    }

    void SpawnAndDump(const CommandContext& ctx)
    {
        if (!Kismet::IsServer(GetWorld())) return;
        const long double delayMs = static_cast<long double>(std::max(200.0f, SafeStof(ctx.Arg(1))));

        //Request all descriptors and all biomes. iterate on all of them including elite versions. Only spawn enemy ONCE , keep track of UClasses.
        std::vector<UEnemyDescriptor*> descriptors{};
        descriptors.reserve(512);
        for (auto* Descriptor : GObjectsOf<UEnemyDescriptor>())
            if (Descriptor->GetName().starts_with("ED_")) descriptors.push_back(Descriptor);
        std::vector<UBiome*> biomes{};
        for (auto* Biome : GObjectsOf<UBiome>())
            biomes.push_back(Biome);

        // Build class → combos map: for each descriptor × (null + all biomes) × (non-elite + elite)
        // call GetEnemyClass; group all (desc,biome,isElite) tuples that resolve to the same UClass.
        struct Combo { std::string desc, biome; bool isElite; };
        struct Entry { UClass* cls = nullptr; std::vector<Combo> combos; };
        std::unordered_map<UClass*, Entry> classMap;
        classMap.reserve(256);

        for (auto* Desc : descriptors)
        {
            auto tryAdd = [&](UBiome* Biome, bool IsElite)
                {
                    UClass* cls = static_cast<UClass*>(Desc->GetEnemyClass(Biome, IsElite));
                    if (!IsValidClass(cls)) return;
                    auto& e = classMap[cls];
                    e.cls = cls;
                    e.combos.push_back({ Desc->GetName(), Biome ? Biome->GetName() : "none", IsElite });
                };
            tryAdd(nullptr, false);
            tryAdd(nullptr, true);
            for (auto* Biome : biomes) { tryAdd(Biome, false); tryAdd(Biome, true); }
        }

        if (classMap.empty()) { warn("[spawndump] No enemy classes resolved"); return; }
        info("[spawndump] {} unique classes from {} descriptors x {} biomes, delay={:.0f}ms",
            classMap.size(), descriptors.size(), biomes.size(), static_cast<double>(delayMs));

        APlayerCharacter* Player = GetLocalPlayer();
        if (!IsValidOf<APlayerCharacter>(Player)) { warn("[spawndump] No local player"); return; }
        const FVector SpawnLoc = Player->K2_GetActorLocation() + Player->GetActorForwardVector() * 400.f;

        // Root JSON object to accumulate all data
        auto rootJson = std::make_shared<nlohmann::json>();
        (*rootJson)["metadata"] = {
            {"totalClasses", classMap.size()},
            {"totalDescriptors", descriptors.size()},
            {"totalBiomes", biomes.size()},
            {"delayMs", static_cast<double>(delayMs)}
        };
        (*rootJson)["enemies"] = nlohmann::json::array();

        // Numeric property dump — float/bool/int family, class chain up to StopAt
        auto dumpNumeric = [](UObject* Obj, UClass* StopAt) -> nlohmann::json
            {
                if (!IsValid(Obj)) return nlohmann::json::object();
                return ExtractNumericPropertiesAsJson(reinterpret_cast<uintptr_t>(Obj), StopAt);
            };

        // Flatten to a stable vector and append a null sentinel.
        // EnqueueThrottled offsets by one: each slot dumps+destroys the previous pawn,
        // logs its combos, then spawns the current one. Sentinel flushes the last pawn.
        auto entries = std::make_shared<std::vector<Entry>>();
        entries->reserve(classMap.size());
        for (auto& [_, e] : classMap) entries->push_back(std::move(e));

        std::vector<Entry*> items;
        items.reserve(entries->size() + 1);
        for (auto& e : *entries) items.push_back(&e);
        items.push_back(nullptr);

        auto prevPawn = std::make_shared<APawn*>(nullptr);
        auto prevEntry = std::make_shared<Entry*>(nullptr);

        EnqueueThrottled<Entry*>(std::move(items),
            std::chrono::milliseconds(static_cast<long long>(delayMs)),
            [prevPawn, prevEntry, entries, dumpNumeric, SpawnLoc, rootJson]
            (Entry*& current, size_t idx, size_t total) -> bool
            {
                // Dump and destroy the previously spawned pawn
                if (*prevEntry && *prevPawn)
                {
                    std::string descriptorName = (*prevEntry)->combos.empty() ? "Unknown" : (*prevEntry)->combos[0].desc;

                    nlohmann::json& enemyEntry = (*rootJson)["Enemies"][descriptorName]["Direct"];

                    // Add all numeric properties
                    auto actorProps = ExtractNumericPropertiesAsJson(reinterpret_cast<uintptr_t>(*prevPawn), AActor::StaticClass());
                    for (auto& [key, value] : actorProps.items())
                        enemyEntry[key] = value;

                    // Add component properties with component name prefix
                    auto comps = (*prevPawn)->K2_GetComponentsByClass(UActorComponent::StaticClass());
                    for (auto* comp : comps)
                    {
                        if (comp->Outer == *prevPawn)
                        {
                            auto compProps = ExtractNumericPropertiesAsJson(
                                reinterpret_cast<uintptr_t>(comp),
                                UActorComponent::StaticClass(),
                                comp->GetName() + "."
                            );
                            for (auto& [key, value] : compProps.items())
                                enemyEntry[key] = value;
                        }
                    }

                    (*prevPawn)->K2_DestroyActor();
                    *prevPawn = nullptr;
                    *prevEntry = nullptr;
                }

                if (!current)
                {
                    // Write JSON to file
                    std::string filename = std::string{ OUTPUT_DIR } + "spawndump_" + std::to_string(std::time(nullptr)) + ".json";
                    std::ofstream outFile = OpenOutput(filename);
                    if (outFile.is_open())
                    {
                        outFile << rootJson->dump(1, '\t');
                        outFile.close();
                        info("[spawndump] Done — {} classes processed, results written to {}", idx, filename);
                    }
                    else
                    {
                        warn("[spawndump] Failed to open file {} for writing", filename);
                    }
                    return true;
                }

                FTransform SpawnTransform{};
                SpawnTransform.Translation = SpawnLoc;
                APawn* Pawn = nullptr;
                SpawnActor<APawn>(current->cls, SpawnTransform, Pawn);
                *prevPawn = Pawn;
                *prevEntry = Pawn ? current : nullptr;
                if (!Pawn)
                    warn("[spawndump] ({}/{}) Spawn failed: {}", idx, total - 1, current->cls->GetName());
                return true;
            }
        );
    }

    void SpawnAndDumpMaterials(const CommandContext& ctx)
    {
        if (!Kismet::IsServer(GetWorld())) return;

        const long double delayMs = static_cast<long double>(std::max(16.0f, SafeStof(ctx.Arg(1))));

        std::vector<UEnemyDescriptor*> descriptors;
        descriptors.reserve(512);
        for (UEnemyDescriptor* Descriptor : GObjectsOf<UEnemyDescriptor>())
            if (Descriptor->GetName().starts_with("ED_"))
                descriptors.push_back(Descriptor);

        struct Entry { UClass* cls = nullptr; std::string desc; };

        std::unordered_map<UClass*, Entry> classMap;
        classMap.reserve(256);
        for (auto* Desc : descriptors)
        {
            UClass* cls = static_cast<UClass*>(Desc->GetEnemyClass(nullptr, false));
            if (!IsValidClass(cls)) continue;
            classMap.emplace(cls, Entry{ cls, Desc->GetName() });
        }

        if (classMap.empty()) { warn("[spawndump] No enemy classes resolved"); return; }
        info("[spawndump] {} unique classes from {} descriptors, delay={:.0f}ms",
            classMap.size(), descriptors.size(), static_cast<double>(delayMs));

        APlayerCharacter* Player = GetLocalPlayer();
        if (!IsValidOf<APlayerCharacter>(Player)) { warn("[spawndump] No local player"); return; }

        const FVector SpawnLoc = Player->K2_GetActorLocation() + Player->GetActorForwardVector() * 400.f;

        auto rootJson = std::make_shared<nlohmann::json>();
        auto entries  = std::make_shared<std::vector<Entry>>();
        entries->reserve(classMap.size());
        for (auto& [_, e] : classMap)
            entries->push_back(std::move(e));

        std::vector<Entry*> items;
        items.reserve(entries->size() + 1);
        for (auto& e : *entries) items.push_back(&e);
        items.push_back(nullptr); // sentinel — triggers file write

        auto prevPawn  = std::make_shared<APawn*>(nullptr);
        auto prevEntry = std::make_shared<Entry*>(nullptr);

        auto delay = std::chrono::milliseconds(static_cast<long long>(delayMs));
        EnqueueThrottled<Entry*>(std::move(items), delay,
            [prevPawn, prevEntry, entries, SpawnLoc, rootJson](Entry*& current, size_t idx, size_t total) -> bool
            {
                // ── Collect materials from the previously spawned pawn ─────
                if (*prevEntry && *prevPawn)
                {
                    const std::string& descName = (*prevEntry)->desc.empty()
                        ? "Unknown" : (*prevEntry)->desc;
                    nlohmann::json& enemyMats = (*rootJson)["Enemies"][descName]["Materials"];

                    uint32 matIndex = 0;
                    TArray<UActorComponent*> comps =
                        (*prevPawn)->K2_GetComponentsByClass(UPrimitiveComponent::StaticClass());
                    for (UActorComponent* actorComp : comps)
                    {
                        auto* comp = Cast<UPrimitiveComponent>(actorComp);
                        if (!comp) continue;
                        for (uint32 i = 0, n = comp->GetNumMaterials(); i < n; ++i)
                        {
                            UMaterialInterface* mat = comp->GetMaterial(i);
                            while (auto* mid = Cast<UMaterialInstanceDynamic>(mat))
                                mat = mid->Parent;
                            enemyMats[matIndex++] = mat ? mat->GetName() : "";
                        }
                    }

                    (*prevPawn)->K2_DestroyActor();
                    *prevPawn  = nullptr;
                    *prevEntry = nullptr;
                }

                // ── Sentinel: all spawns done, write output ───────────────
                if (!current)
                {
                    std::string filename = std::string{ OUTPUT_DIR } + "MaterialDump.json";
                    std::ofstream outFile = OpenOutput(filename);
                    if (!outFile.is_open())
                        return warn("[spawndump] Failed to open file {} for writing", filename), true;
                    outFile << rootJson->dump(1, '\t');
                    outFile.close();
                    info("[spawndump] Done — {} classes processed, results written to {}", idx, filename);
                    return true;
                }

                // ── Spawn next enemy ──────────────────────────────────────
                static FTransform SpawnTransform{};
                SpawnTransform.Translation = SpawnLoc;

                APawn* Pawn = nullptr;
                SpawnActor<APawn>(current->cls, SpawnTransform, Pawn);
                *prevPawn  = Pawn;
                *prevEntry = Pawn ? current : nullptr;
                if (!Pawn)
                    warn("[spawndump] ({}/{}) Spawn failed: {}", idx, total - 1, current->cls->GetName());
                return true;
            });
    }

    void DumpDescriptors(const CommandContext& = State::dummyCtx)
    {
        // UEnum* for a named field via FEnumProperty / FByteProperty reflection
        auto getFieldEnum = [](UClass* Cls, const char* Name) -> UEnum*
            {
                FName needle(ToWide(std::string(Name)).c_str());
                for (FField* field : FFieldRange(Cls->ChildProperties))
                {
                    if (field->Name != needle) continue;
                    if (auto* ep = FieldCast::Cast<FEnumProperty>(field)) return ep->Enum;
                    if (auto* bp = FieldCast::Cast<FByteProperty>(field))  return bp->Enum;
                }
                return nullptr;
            };

        // Enum integer → short name via UEnum::Names, strips "EnumClass::" prefix
        auto enumName = [](UEnum* E, int64 Val) -> std::string
            {
                if (!E) return std::to_string(Val);
                for (int32 i = 0; i < E->Names.Num(); ++i)
                {
                    if (E->Names[i].Value() != Val) continue;
                    std::string full = E->Names[i].Key().ToString();
                    const auto sep = full.rfind("::");
                    return sep != std::string::npos ? full.substr(sep + 2) : full;
                }
                return std::to_string(Val);
            };

        // One FField walk per enum type, not per descriptor
        UClass* DescClass = UEnemyDescriptor::StaticClass();
        UEnum* SigEnum = getFieldEnum(DescClass, "EnemySignificance");
        UEnum* VetEnum = getFieldEnum(DescClass, "VeteranScaling");

        nlohmann::json root = nlohmann::json::object();

        for (auto* Desc : GObjectsOf<UEnemyDescriptor>())
        {
            if (!Desc->GetName().starts_with("ED_")) continue;

            nlohmann::json d;

            d["DifficultyRating"] = Desc->DifficultyRating;
            d["MinSpawnCount"] = Desc->MinSpawnCount;
            d["MaxSpawnCount"] = Desc->MaxSpawnCount;
            d["Rarity"] = Desc->Rarity;
            d["SpawnAmountModifier"] = Desc->SpawnAmountModifier;
            d["SpawnSpread"] = Desc->SpawnSpread;
            d["CanBeUsedForConstantPressure"] = Desc->CanBeUsedForConstantPressure;
            d["CanBeUsedInEncounters"] = Desc->CanBeUsedInEncounters;
            d["UsesSpawnRarityModifiers"] = Desc->UsesSpawnRarityModifiers;

            d["Significance"] = enumName(SigEnum, static_cast<int64>(Desc->EnemySignificance));
            d["VeteranScaling"] = enumName(VetEnum, static_cast<int64>(Desc->VeteranScaling));
            d["UsesVeteranLarge"] = (Desc->VeteranScaling == EVeteranScaling::LargeEnemy);

            nlohmann::json vets = nlohmann::json::array();
            for (int32 i = 0; i < Desc->VeteranClasses.Num(); ++i)
            {
                auto* v = Desc->VeteranClasses[i];
                if (v && IsValid(v)) vets.push_back(v->GetName());
            }
            d["VeteranClasses"] = std::move(vets);

            root[Desc->GetName()] = std::move(d);
        }

        const std::string path = std::string{ OUTPUT_DIR } + "EnemyDescriptors.json";
        std::ofstream f = OpenOutput(path);
        if (f) { f << root.dump(2); info("[dumpdescriptors] {} descriptors → {}", root.size(), path); }
        else warn("[dumpdescriptors] Failed to open {}", path);
    }

    void ToggleGodmode(const CommandContext& = State::dummyCtx)
    {
        auto* Local = GetLocalPlayer();
        if (!Kismet::IsValid(Local)) return;
        trace("[cmd:godmode] Old CanTakeDamage: {}", Local->HealthComponent->canTakeDamage);
        Local->HealthComponent->ToggleCanTakeDamage();
        info("[cmd:godmode] God mode enabled for local player.");
    }

    void ToggleInfiniteAmmo(const CommandContext& = State::dummyCtx)
    {
        auto* LocalPlayer = GetLocalPlayer();
        if (!LocalPlayer || !Kismet::IsValid(LocalPlayer->InventoryComponent)) 
        {
            State::infiniteAmmoEnabled = false;
            return;
        }
        State::infiniteAmmoEnabled = !State::infiniteAmmoEnabled;
        if (State::infiniteAmmoEnabled)
        {
            for (auto* weapon : Weapons::GetAllAmmoWeapons(LocalPlayer))
                if (Kismet::IsValid(weapon))
                {
                    State::OldShotCost[weapon] = weapon->ShotCost; weapon->ShotCost = 0;
                }
        }
        else
        {
            for (auto* weapon : Weapons::GetTrackedAmmoWeapons())
                if (Kismet::IsValid(weapon) && State::OldShotCost.count(weapon))
                    weapon->ShotCost = State::OldShotCost[weapon];
            State::OldShotCost.clear();
        }
        info("[cmd:infiniteammo] {}", State::infiniteAmmoEnabled ? "Enabled" : "Disabled");
    }
}

namespace Binds {
    void Crash() {
        Commands::Crash();
    }
    struct BaseGunStats{};

    std::unordered_map<FName, BaseGunStats> Stats;
    void AimbotDisable() {}

    struct WpTargetResult
    {
        FVector pos        = {};
        bool hasWeakpoints = false; // enemy has WP bodies in physics asset at all
        bool anyVisible    = false; // at least one visible, alive WP was found
    };

    // Scans all mesh components for visible, alive weakpoint bodies.
    // hasWeakpoints=true even if all are occluded or destroyed (enemy IS a WP enemy).
    // anyVisible=true only if the returned pos is a valid WP target.
    static WpTargetResult GetWeakpointTarget(AFSDPawn* Enemy, const std::vector<USkeletalMeshComponent*>& Meshes,
                                              const FVector& CamLoc, const FVector& Forward,
                                              APlayerCharacter* LocalPlayer)
    {
        using namespace ObjectCast;
        static constexpr float kDeg2Rad = 3.14159265f / 180.f;

        WpTargetResult result;
        FVector BestPos  = {};
        float   BestMult = -1.f;
        float   BestDot  = -2.f;

        for (USkeletalMeshComponent* Mesh : Meshes)
        {
            if (!Mesh) continue;
            auto* SkelAsset = Mesh->SkeletalMesh;
            if (!SkelAsset) continue;
            auto* PhysAsset = SkelAsset->PhysicsAsset;
            if (!PhysAsset) continue;

            for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); ++i)
            {
                USkeletalBodySetup* Body = PhysAsset->SkeletalBodySetups[i];
                if (!Body || !Body->PhysMaterial) continue;

                UFSDPhysicalMaterial* FSDMat = Cast<UFSDPhysicalMaterial>(Body->PhysMaterial);
                if (!FSDMat || !FSDMat->IsWeakPoint) continue;

                result.hasWeakpoints = true;

                auto WpState = Helpers::GetBreakableWpState(Enemy, Mesh, Body->BoneName, FSDMat);
                if (WpState.isBreakable && WpState.isDestroyed) continue;

                const FVector Center = Mesh->GetSocketLocation(Body->BoneName);
                const float   R      = Helpers::GetBodyRadius(Body);

                // Multipoint: try center then ±R on each world axis.
                // First candidate visible from CamLoc wins as the aim position.
                static const std::array<FVector, 7> kOffsets = {{
                    {0,0,0}, {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
                }};
                FVector VisPos  = {};
                bool    bVisible = false;
                for (const auto& Off : kOffsets)
                {
                    FVector Candidate = { Center.X + Off.X*R, Center.Y + Off.Y*R, Center.Z + Off.Z*R };
                    if (Helpers::IsWeakpointVisible(LocalPlayer, Enemy, CamLoc, Candidate, Body->BoneName))
                        { VisPos = Candidate; bVisible = true; break; }
                }
                if (!bVisible) continue;

                FVector Dir = VisPos - CamLoc;
                const float Dist = std::sqrt(Dir.X*Dir.X + Dir.Y*Dir.Y + Dir.Z*Dir.Z);
                if (Dist < 1.f) continue;
                Dir.X /= Dist; Dir.Y /= Dist; Dir.Z /= Dist;

                const float Dot  = Forward.X*Dir.X + Forward.Y*Dir.Y + Forward.Z*Dir.Z;
                const float Mult = FSDMat->DamageMultiplier;

                if (Mult > BestMult || (Mult >= BestMult && Dot > BestDot))
                    { BestMult = Mult; BestDot = Dot; BestPos = VisPos; result.anyVisible = true; }
            }
        }

        result.pos = result.anyVisible ? BestPos : Enemy->K2_GetActorLocation();
        return result;
    }

    void Aimbot() {
        using namespace ObjectCast;
        static constexpr float kDeg2Rad = 3.14159265f / 180.f;
        struct Config {
            float FOV        = 90.f;   // aim cone half-angle in degrees
            float DeadzoneDeg = 2.f;   // don't correct if already within this many degrees
        };
        static Config Config;

        State::AimbotHasTarget = false;
        if (IsOnSpaceRig()) return;

        APlayerCharacter* LocalPlayer = GetLocalPlayer();
        if (!IsValidOf<APlayerCharacter>(LocalPlayer)) return;

        AFSDPlayerController* LocalController = Cast<AFSDPlayerController>(LocalPlayer->Controller);
        if (!IsValidOf<AFSDPlayerController>(LocalController)) return;

        APlayerCameraManager* CamMgr = LocalController->PlayerCameraManager;
        if (!CamMgr) return;

        UInventoryComponent* Inventory = LocalPlayer->InventoryComponent;
        if (!Inventory) return;

        AItem* Equipped = Inventory->GetEquippedItem();
        if (!Equipped) return;
        
        Helpers::FDamageInfo DamageInfo;
        
        if (UDamageComponent* DamageComponent = GetComponent<UDamageComponent>(Equipped))
            Helpers::ExtractDamageInfo(DamageComponent, DamageInfo);
        else
        {
            UProjectileLauncherComponent* Launcher = GetComponent<UProjectileLauncherComponent>(Equipped);
            if (!Launcher) return;

            if (!GetDamageInfoFromProjectileClass(Launcher->ProjectileClass, DamageInfo) ) return;
        }
        if (!DamageInfo.IsValid()) return;

        std::vector<AFSDPawn*> Enemies = GetAliveNonFriendlies();
        if (Enemies.empty()) return;

        const FVector  CamLoc = CamMgr->GetCameraLocation();
        const FRotator CamRot = CamMgr->GetCameraRotation();

        const float PR = CamRot.Pitch * kDeg2Rad;
        const float YR = CamRot.Yaw   * kDeg2Rad;
        const FVector Forward{
            std::cos(PR) * std::cos(YR),
            std::cos(PR) * std::sin(YR),
            std::sin(PR)
        };

        const float HalfFOVCos = std::cos(Config.FOV * 0.5f * kDeg2Rad);

        // Collect all candidates inside the FOV cone, sorted closest-to-crosshair first.
        struct Candidate { float dot; AFSDPawn* enemy; };
        std::vector<Candidate> candidates;
        for (AFSDPawn* Enemy : Enemies)
        {
            if (!IsValidOf<AFSDPawn>(Enemy)) continue;
            FVector ToEnemy = Enemy->K2_GetActorLocation() - CamLoc;
            const float Dist = std::sqrt(ToEnemy.X*ToEnemy.X + ToEnemy.Y*ToEnemy.Y + ToEnemy.Z*ToEnemy.Z);
            if (Dist < 0.01f) continue;
            ToEnemy.X /= Dist; ToEnemy.Y /= Dist; ToEnemy.Z /= Dist;
            const float Dot = Forward.X*ToEnemy.X + Forward.Y*ToEnemy.Y + Forward.Z*ToEnemy.Z;
            if (Dot >= HalfFOVCos) candidates.push_back({Dot, Enemy});
        }
        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.dot > b.dot; });

        // Pick the first candidate that either has no weakpoints (aim at center)
        // or has at least one visible, alive weakpoint.
        AFSDPawn* BestTarget = nullptr;
        FVector   AimPos     = {};
        for (auto& c : candidates)
        {
            auto meshes = Helpers::GetEnemyMeshes(c.enemy);
            if (meshes.empty()) {
                // No mesh data — fall back to actor center.
                BestTarget = c.enemy;
                AimPos     = c.enemy->K2_GetActorLocation();
                break;
            }
            auto wp = GetWeakpointTarget(c.enemy, meshes, CamLoc, Forward, LocalPlayer);
            if (wp.hasWeakpoints && !wp.anyVisible) continue; // WP enemy, but none visible
            BestTarget = c.enemy;
            AimPos     = wp.pos;
            break;
        }
        if (!BestTarget) return;

        const FRotator AimRot = UKismetMathLibrary::FindLookAtRotation(CamLoc, AimPos);

        // Deadzone: skip correction if already aimed within DeadzoneDeg of the target.
        {
            const float AP = AimRot.Pitch * kDeg2Rad, AY = AimRot.Yaw * kDeg2Rad;
            const FVector AimDir{ std::cos(AP)*std::cos(AY), std::cos(AP)*std::sin(AY), std::sin(AP) };
            const float dotToAim = Forward.X*AimDir.X + Forward.Y*AimDir.Y + Forward.Z*AimDir.Z;
            if (dotToAim >= std::cos(Config.DeadzoneDeg * kDeg2Rad)) return;
        }

        State::AimbotHasTarget = true;
        const FRotator CtrlRot = LocalController->GetControlRotation();
        LocalPlayer->K2_SetActorRotation({0.f, AimRot.Yaw, 0.f}, false);
        LocalController->SetControlRotation({AimRot.Pitch, AimRot.Yaw, CtrlRot.Roll});
    }

    void EnableAimbot() {
        if (State::AimbotEnabled) return;
        State::AimbotEnabled = true;
        State::AimbotHandle = GameHooks::OnProcessEventByNameAndClass(
            "ReceiveTick", APlayerCameraManager::StaticClass(),
            [](UObject* Obj, UFunction*, void*) {
                using namespace ObjectCast;
                APlayerCharacter* Player = GetLocalPlayer();
                if (!IsValidOf<APlayerCharacter>(Player)) return;
                AFSDPlayerController* Ctrl = Cast<AFSDPlayerController>(Player->Controller);
                if (!IsValidOf<AFSDPlayerController>(Ctrl)) return;
                if (static_cast<APlayerCameraManager*>(Obj) != Ctrl->PlayerCameraManager) return;
                Aimbot();
            },
            GameHooks::ClassMatchMode::ExactOrSubclass,
            GameHooks::ExecutionTiming::Before,
            GameHooks::ExecutionMode::CallOriginal);
    }

    void DisableAimbot() {
        State::AimbotEnabled   = false;
        State::AimbotHasTarget = false;
        GameHooks::RemoveHook(State::AimbotHandle);
        State::AimbotHandle = 0;
    }

    void ToggleRecoilControl() {
        State::RecoilEnabled  = !State::RecoilEnabled;
        State::RCSInitialized = false;

        if (State::RecoilEnabled) {
            struct RCSConfig { float Factor = 1.0f; };
            static RCSConfig RCSConfig;

            // UE4 pitch/yaw live in [0,360): normalize to [-180,180] so
            // arithmetic and clamps stay geometrically correct.
            auto normP = [](float a) -> float {
                a = std::fmod(a, 360.f);
                if (a > 180.f)  a -= 360.f;
                if (a < -180.f) a += 360.f;
                return a;
            };

            EnqueueWhile([normP]() -> bool {
                if (!State::RecoilEnabled) return false;

                using namespace ObjectCast;
                APlayerCharacter* Player = GetLocalPlayer();
                if (!IsValidOf<APlayerCharacter>(Player)) return true;

                AFSDPlayerController* Ctrl = Cast<AFSDPlayerController>(Player->Controller);
                if (!IsValidOf<AFSDPlayerController>(Ctrl)) return true;

                APlayerCameraManager* CamMgr = Ctrl->PlayerCameraManager;
                if (!CamMgr) return true;

                const FRotator ctrlRot = Ctrl->GetControlRotation();
                const FRotator camRot  = CamMgr->GetCameraRotation();
                const float ctrlPitch  = normP(ctrlRot.Pitch);
                const float camPitch   = normP(camRot.Pitch);
                const float ctrlYaw    = normP(ctrlRot.Yaw);
                const float camYaw     = normP(camRot.Yaw);

                // EnqueueWhile fires on every ProcessEvent — many times per
                // rendered frame. Camera updates once per frame. Gate on
                // cam values changing so we apply exactly one correction
                // per camera frame, not N (which would compound to ±90).
                static float lastCamPitch = 1e9f;
                static float lastCamYaw   = 1e9f;

                if (!State::RCSInitialized) {
                    lastCamPitch            = camPitch;
                    lastCamYaw              = camYaw;
                    State::RCSDesiredPitch  = ctrlPitch;
                    State::RCSPrevCtrlPitch = ctrlPitch;
                    State::RCSDesiredYaw    = ctrlYaw;
                    State::RCSPrevCtrlYaw   = ctrlYaw;
                    State::RCSInitialized   = true;
                    return true;
                }
                if (camPitch == lastCamPitch && camYaw == lastCamYaw) return true;
                lastCamPitch = camPitch;
                lastCamYaw   = camYaw;

                // ── Pitch ──────────────────────────────────────────────
                // Fold in mouse input (ctrl change we didn't write).
                State::RCSDesiredPitch += normP(ctrlPitch - State::RCSPrevCtrlPitch);
                State::RCSDesiredPitch  = std::clamp(State::RCSDesiredPitch, -90.f, 90.f);

                const float pitchOffset = camPitch - ctrlPitch;
                const float newPitch = std::clamp(
                    State::RCSDesiredPitch - pitchOffset * RCSConfig.Factor,
                    -90.f, 90.f);

                // ── Yaw ────────────────────────────────────────────────
                // Same pattern, but no [-90,90] clamp — yaw wraps full circle.
                // Normalize desired/offset to handle the 0/360 seam.
                State::RCSDesiredYaw += normP(ctrlYaw - State::RCSPrevCtrlYaw);
                State::RCSDesiredYaw  = normP(State::RCSDesiredYaw);

                const float yawOffset = normP(camYaw - ctrlYaw);
                const float newYaw    = normP(State::RCSDesiredYaw - yawOffset * RCSConfig.Factor);

                FRotator rot = ctrlRot;
                rot.Pitch = newPitch;
                rot.Yaw   = newYaw;
                Ctrl->SetControlRotation(rot);
                State::RCSPrevCtrlPitch = newPitch;
                State::RCSPrevCtrlYaw   = newYaw;

                return true;
            });
        }
        spdlog::info("[recoil] {}", State::RecoilEnabled ? "ON" : "OFF");
    }

    void ToggleSilentAim() {
        State::SilentAimEnabled = !State::SilentAimEnabled;
        if (State::SilentAimEnabled) {
            State::SilentAimHandle = GameHooks::OnProcessEventByNameAndClass(
                "Fire", UWeaponFireComponent::StaticClass(),
                [](UObject* Obj, UFunction*, void* Parms) {
                    using namespace ObjectCast;
                    if (!Parms) return;

                    auto* Weapon = Cast<AAmmoDrivenWeapon>(Obj->Outer);
                    if (!IsValidOf<AAmmoDrivenWeapon>(Weapon)) return;

                    APlayerCharacter* Player = GetLocalPlayer();
                    if (!IsValidOf<APlayerCharacter>(Player)) return;
                    if (Weapon->Character != Player || !Weapon->IsEquipped) return;

                    AFSDPlayerController* Ctrl = Cast<AFSDPlayerController>(Player->Controller);
                    if (!IsValidOf<AFSDPlayerController>(Ctrl)) return;

                    APlayerCameraManager* CamMgr = Ctrl->PlayerCameraManager;
                    if (!CamMgr) return;

                    if (IsOnSpaceRig()) return;

                    std::vector<AFSDPawn*> Enemies = GetAliveNonFriendlies();
                    if (Enemies.empty()) return;

                    static constexpr float kDeg2Rad = 3.14159265f / 180.f;
                    struct Config { float FOV = 30.f; };
                    static Config Config;

                    const FVector  CamLoc = CamMgr->GetCameraLocation();
                    const FRotator CamRot = CamMgr->GetCameraRotation();
                    const float PR = CamRot.Pitch * kDeg2Rad;
                    const float YR = CamRot.Yaw   * kDeg2Rad;
                    const FVector Forward{
                        std::cos(PR) * std::cos(YR),
                        std::cos(PR) * std::sin(YR),
                        std::sin(PR)
                    };

                    const float HalfFOVCos = std::cos(Config.FOV * 0.5f * kDeg2Rad);

                    struct Candidate { float dot; AFSDPawn* enemy; };
                    std::vector<Candidate> candidates;
                    for (AFSDPawn* Enemy : Enemies)
                    {
                        if (!IsValidOf<AFSDPawn>(Enemy)) continue;
                        FVector ToEnemy = Enemy->K2_GetActorLocation() - CamLoc;
                        const float Dist = std::sqrt(ToEnemy.X*ToEnemy.X + ToEnemy.Y*ToEnemy.Y + ToEnemy.Z*ToEnemy.Z);
                        if (Dist < 0.01f) continue;
                        ToEnemy.X /= Dist; ToEnemy.Y /= Dist; ToEnemy.Z /= Dist;
                        const float Dot = Forward.X*ToEnemy.X + Forward.Y*ToEnemy.Y + Forward.Z*ToEnemy.Z;
                        if (Dot >= HalfFOVCos) candidates.push_back({Dot, Enemy});
                    }
                    if (candidates.empty()) return;
                    std::sort(candidates.begin(), candidates.end(),
                        [](const Candidate& a, const Candidate& b) { return a.dot > b.dot; });

                    AFSDPawn* BestTarget = nullptr;
                    FVector   AimPos     = {};
                    for (auto& c : candidates)
                    {
                        auto meshes = Helpers::GetEnemyMeshes(c.enemy);
                        if (meshes.empty()) { BestTarget = c.enemy; AimPos = c.enemy->K2_GetActorLocation(); break; }
                        auto wp = GetWeakpointTarget(c.enemy, meshes, CamLoc, Forward, Player);
                        if (wp.hasWeakpoints && !wp.anyVisible) continue;
                        BestTarget = c.enemy;
                        AimPos     = wp.pos;
                        break;
                    }
                    if (!BestTarget) return;

                    // Redirect the shot: overwrite Direction in-place.
                    // Origin is the barrel/muzzle position from the Parms buffer.
                    auto* p = reinterpret_cast<Params::WeaponFireComponent_Fire*>(Parms);
                    FVector Dir = AimPos - p->Origin;
                    const float Dist = std::sqrt(Dir.X*Dir.X + Dir.Y*Dir.Y + Dir.Z*Dir.Z);
                    if (Dist < 1.f) return;
                    Dir.X /= Dist; Dir.Y /= Dist; Dir.Z /= Dist;
                    p->Direction.X = Dir.X;
                    p->Direction.Y = Dir.Y;
                    p->Direction.Z = Dir.Z;
                },
                ClassMatchMode::ExactOrSubclass,
                ExecutionTiming::Before,
                ExecutionMode::CallOriginal);
        } else {
            GameHooks::RemoveHook(State::SilentAimHandle);
            State::SilentAimHandle = 0;
        }
        spdlog::info("[silent aim] {}", State::SilentAimEnabled ? "ON" : "OFF");
    }

    void InfiniteAmmo() {
        Commands::ToggleInfiniteAmmo();
    }
    void RegisterKeybinds() {
        using enum Key;
        using enum Trigger;
        using enum Focus;

        KeyBindings::RegisterGameThread(
            Key::F4,
            Mod::None, 
            Binds::Crash,
            BindingOptions{
                Press,
                Game,
                true
            }
        );

        KeyBindings::RegisterGameThread(
            MouseLeft,
            Mod::None,
            Binds::EnableAimbot,
            BindingOptions{ Press, Game, false }
        );

        KeyBindings::RegisterGameThread(
            MouseLeft,
            Mod::None,
            Binds::DisableAimbot,
            BindingOptions{ Release, Game, false }
        );

        KeyBindings::RegisterGameThread(
            I,
            Mod::Ctrl,
            Binds::InfiniteAmmo,
            BindingOptions
            {
                Press,
                Game,
                true
            }
        );

        KeyBindings::RegisterGameThread(
            Key::R,
            Mod::Ctrl,
            Binds::ToggleRecoilControl,
            BindingOptions
            {
                Press,
                Game,
                false
            }
        );

        KeyBindings::RegisterGameThread(
            Key::S,
            Mod::Ctrl,
            Binds::ToggleSilentAim,
            BindingOptions
            {
                Press,
                Game,
                false
            }
        );
    }

}

// =========================================================================
// Public interface
// =========================================================================

void RegisterCommands(CommandHandler& handler)
{
    using namespace VarSystem;

    // Chat
    handler.Register("logchat", Commands::LogChat, "Chat", R"(Toggle chat message logging)");
    handler.Register("say", Commands::Say, "Chat", R"(Send message to chat: say [sender] <message>)");

    // Enemies
    handler.Register("dumpdescriptors", Commands::DumpDescriptors, "Enemies", R"(Dump all ED_* descriptor controls to C:\Dumper-7\DescriptorDump.json)");
    handler.Register("fearall", Commands::FearAll, "Enemies", R"(Instantly fear all enemies (requires Coil Gun equipped))");
    handler.Register("wpinfo", Commands::WPInfo, "Enemies", R"(Dump weakpoint physics bodies for the nearest enemy)");
    handler.Register("spawndump", Commands::SpawnAndDump, "Enemies", R"(Dump float/bool/int props for all enemy descriptors x biomes: spawndump [delay_ms])");
    handler.Register("spawnenemies", Commands::SpawnEnemies, "Enemies", R"(Spawn enemies: spawnenemies <descriptor> <count> <x> <y> <z>)");
    handler.Register("spawnmatdump", Commands::SpawnAndDumpMaterials, "Enemies", R"(Dump material names for all enemy descriptors x biomes: spawnmatdump [delay_ms])");

    // Inspection
    handler.Register("findclass", Commands::FindClass, "Inspection", R"(Find classes by name)");
    handler.Register("inventory", Commands::Inventory, "Inspection", R"(Dump local player inventory)");
    handler.Register("prop", Commands::Prop, "Inspection", R"(prop <cdo|obj> <name> <dump|get|set|list> [...])");
    handler.Register("scanall", Commands::ScanAllClasses, "Inspection", R"(Scan all classes for server RPCs and log them)");
    handler.Register("scandmg", Commands::ScanDamageMeeterMod, "Inspection", R"(Scan damage meeter mod for usable actors)");
    handler.Register("scanfuncs", Commands::ScanReplicated, "Inspection", R"(Scan for usable replicated functions)");

    // Player
    handler.Register("coilcarve", Commands::CoilCarve, "Player", R"(Carve terrain with the Coil Gun: coilcarve <x1> <y1> <z1> <x2> <y2> <z2> <power>)");
    handler.Register("godmode", Commands::ToggleGodmode, "Player", R"(Toggle god mode for local player)");
    handler.Register("infiniteammo", Commands::ToggleInfiniteAmmo, "Player", R"(Toggle infinite ammo for local player)");
    handler.Register("readname", Commands::ReadName, "Player", R"(Read the name of the local player)");
    handler.Register("rename", Commands::Rename, "Player", R"(Rename the local player: rename <new_name>)");
    handler.Register("recoil", [](const CommandContext&) { Binds::ToggleRecoilControl(); }, "Player", R"(Toggle recoil compensation (cancels expected RecoilPitch per shot via OnWeaponFired))");
    handler.Register("silentaim", [](const CommandContext&) { Binds::ToggleSilentAim(); }, "Player", R"(Toggle silent aim (redirects Fire direction to nearest weakpoint, no view rotation) [Ctrl+S])");

    // System
    handler.Register("beginplay", Commands::BeginPlay, "System", R"(Toggle BeginPlay spawn watcher)");
    handler.Register("call", Commands::Call, "System", R"(Call a server RPC: call <FunctionName> [args...])");
    handler.Register("exec", Commands::Exec, "System", R"(Execute a console command: exec <command>)");
    handler.Register("ignoreproxy", Commands::IgnoreProxy, "System", R"(Toggle ProxyMod hook)");
    handler.Register("stoptick", Commands::StopTick, "System", R"(Toggle tick event logging)");
    handler.Register("logallevents", Commands::LogProcessEvents, "System", R"(Toggle logging of ALL ProcessEvent calls (very verbose))");
    handler.Register("json", Commands::MakeJSON, "System", R"(Diagnose JsonHook: print hook state; json [string] also runs direct C++ parse)");

    // Teleport
    handler.Register("hidesound", Commands::HideSound, "Teleport", R"(Teleport out of hearing range on the space rig)");
    handler.Register("ptp", Commands::PlayerTeleport, "Teleport", R"(Player teleport: ptp <name> | target <name> | dest <name>)");
    handler.Register("tp", Commands::Teleport, "Teleport", R"(Teleport: tp <x> <y> <z> [--rel] [--sweep])");
    handler.Register("tpall", Commands::TeleportAllToSelf, "Teleport", R"(Teleport all players to self)");

    // Troll
    handler.Register("67", Commands::SixSeven, "Troll", R"(Send 'Six Seven' chat message to twerking players)");
    handler.Register("cleartwerkers", Commands::ClearTwerkers, "Troll", R"(Clear the vanity dwarfs)");
    handler.Register("crash", Commands::Crash, "Troll", R"(Crash host)");
    handler.Register("dance", Commands::Dance, "Troll", R"(Show local player dance state)");
    handler.Register("dancer", Commands::SpawnDancer, "Troll", R"(Spawn a dancing dwarf on self)");
    handler.Register("troll", Commands::Troll, "Troll", R"(Troll all players with random teleports)");
    handler.Register("twerk", Commands::Twerk, "Troll", R"(Toggle anti-twerk filter)");
    handler.Register("untwerk", Commands::DoUntwerk, "Troll", R"(Stop all other players from twerking)");

    // Variables
    handler.Register("get", Commands::CmdGet, "Variables", R"(Get a variable: get <n>)");
    handler.Register("set", Commands::CmdSet, "Variables", R"(Set a variable: set <n> <value>)");
    handler.Register("unset", Commands::CmdUnset, "Variables", R"(Delete a variable: unset <n>)");
    handler.Register("vars", Commands::CmdVars, "Variables", R"(List all variables)");

    // Keybindings
    KeyBindings::RegisterCommands(handler);
    Binds::RegisterKeybinds();
    
}

void SendCommandList(const CommandContext& ctx, const CommandHandler& handler)
{
    if (!g_pRespBuffer || g_pRespBuffer->ready.load(std::memory_order_acquire))
        return;

    CommandsResponse cr{};
    for (const auto& [name, entry] : handler.GetEntries())
    {
        if (cr.count >= MAX_CMD_COUNT) break;
        auto& out = cr.cmds[cr.count++];
        strncpy_s(out.name, name.c_str(), MAX_CMD_NAME - 1);
        strncpy_s(out.desc, entry.description.c_str(), MAX_CMD_DESC - 1);
    }

    g_pRespBuffer->type = ResponseType::Commands;
    memcpy(&g_pRespBuffer->data.commands, &cr, sizeof(cr));
    g_pRespBuffer->seq.store(ctx.seq, std::memory_order_release);
    g_pRespBuffer->ready.store(true, std::memory_order_release);
    SetEvent(g_hRespEvent);
}

void InitDefaultCallbacks()
{
    Commands::LogChat();
    Commands::Twerk();
    Commands::BeginPlay();
    RegisterBuiltinBindings();
    JsonHook::Setup();

    TickSystem::SetTickableFunction_AsIntervalMs(Tickables::DeletePitjaws, 5000L);
    TickSystem::SetTickableFunction_AsFrequencyHz(Tickables::LockPlayers, 1L);
    //TickSystem::SetTickableFunction_AsFrequencyHz(Tickables::SpawnTwerk,Twerking::GetSpawnTwerkTrailFrequencyHz);
    //TickSystem::SetTickableFunction_AsFrequencyHz(Tickables::FearAura, 2L);
    //TickSystem::SetTickableFunction_AsIntervalMs(Tickables::LokiAbuse, 2500L);
}

void ResetCallbackHandles()
{
    State::DanceCallback =
        State::TickCallback =
        State::ClientChatCallback =
        State::ServerChatCallback =
        State::ProxyModCallback =
        State::BeginPlayCallback =
        0;
}

#pragma pop_macro("EOF")