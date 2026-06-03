// Commands.cpp — All game command implementations and callback handlers.
#include "Commands.h"
#include "ModManager.h"
#include "Library.h"
#include "Common.h"
#include "Aim.h"
#include "Lib_Utils.h"
#include "Lib_NetLogConfig.h"
#include "SharedCommands.h"

#include <array>
#include <atomic>
#include <unordered_set>
#include <chrono>
#include <optional>
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

using namespace SDK;
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
    std::vector<SDK::AAmmoDrivenWeapon*> GetAllAmmoWeapons(SDK::APlayerCharacter* LocalPlayer);
    std::vector<SDK::AAmmoDrivenWeapon*> GetTrackedAmmoWeapons();
    void LokiSphereExplosions(SDK::AWPN_LockOnRifle_C* Loki, float R, int N);
    void UpgradeLoki(SDK::AWPN_LockOnRifle_C* Loki);
    void NukeEnemiesWithECR(SDK::AWPN_LockOnRifle_C* Loki);
    void ECRTrail(SDK::AWPN_LockOnRifle_C* Loki, size_t Count);
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

inline nlohmann::json ExtractNumericPropertiesAsJson(uintptr_t Base, SDK::UClass* StopAt, const std::string& prefix = "")
{
    nlohmann::json result = nlohmann::json::object();
    if (!Base) return result;

    const auto chain = BuildClassChain(reinterpret_cast<SDK::UObject*>(Base)->Class, StopAt);
    for (SDK::UStruct* level : chain)
    {
        SDK::UClass* cls = ObjectCast::Cast<SDK::UClass>(level);
        if (!cls) continue;

        for (SDK::FField* field : SDK::FFieldRange(cls->ChildProperties))
        {
            bool numeric = false;
            std::string typePrefix;

            SDK::FieldCast::Visit(field, [&](auto* p)
                {
                    using T = std::remove_pointer_t<decltype(p)>;
                    if (std::is_same_v<T, SDK::FFloatProperty>) { numeric = true; typePrefix = "Float"; }
                    else if (std::is_same_v<T, SDK::FDoubleProperty>) { numeric = true; typePrefix = "Float"; }
                    else if (std::is_same_v<T, SDK::FBoolProperty>) { numeric = true; typePrefix = "Bool"; }
                    else if (std::is_same_v<T, SDK::FIntProperty>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, SDK::FInt8Property>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, SDK::FInt16Property>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, SDK::FInt64Property>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, SDK::FUInt16Property>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, SDK::FUInt32Property>) { numeric = true; typePrefix = "Int"; }
                    else if (std::is_same_v<T, SDK::FUInt64Property>) { numeric = true; typePrefix = "Int"; }
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
    return IsValidOf<SDK::APlayerCharacter>(GetLocalPlayer());
}

namespace Enum {
    constexpr int8 EnemyHealthIndex(SDK::EEnemyHealthScaling Scale)
    {
        switch (Scale)
        {
        case SDK::EEnemyHealthScaling::SmallEnemy:      return 0;
        case SDK::EEnemyHealthScaling::LargeEnemy:      return 1;
        case SDK::EEnemyHealthScaling::ExtraLargeEnemy: return 2;
        case SDK::EEnemyHealthScaling::ExtraLargeEnemyB:return 3;
        case SDK::EEnemyHealthScaling::ExtraLargeEnemyC:return 4;
        case SDK::EEnemyHealthScaling::ExtraLargeEnemyD:return 5;
        case SDK::EEnemyHealthScaling::NoScaling:       return 6;
        default: return -1; // error
        }
    }
}

namespace Helpers {

    const std::array<float, static_cast<int>(SDK::EEnemyHealthScaling::EEnemyHealthScaling_MAX)> GetCurrentEnemyHealthScalings() {
        std::array<float, static_cast<int>(SDK::EEnemyHealthScaling::EEnemyHealthScaling_MAX)> Scalings{};

        SDK::UDifficultySetting* Difficulty = CastChecked<SDK::AFSDGameState>(SDK::UGameplayStatics::GetGameState(GetWorld()))->GetCurrentDifficultySetting();
        int32 Idx = GameLib::GetNumPlayers(GetWorld(), false) >= 4 ? 3 : GameLib::GetNumPlayers(GetWorld(), false);

        using namespace Enum;
        using enum SDK::EEnemyHealthScaling;

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
    CallbackHandle DanceCallback = 0;   // kept: also referenced by DanceIntercept (SetExecutionMode)

    bool autoCrasherEnabled = false;
    bool infiniteAmmoEnabled = false;
    std::unordered_map<SDK::UObject*, int> OldShotCost;

    struct ScannedFunction
    {
        SDK::UFunction* Func = nullptr;
        SDK::UObject* Owner = nullptr;
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

        if (!IsValidOf<SDK::APlayerCharacter>(player) || !GetWorld()) {
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
            const long double speed = (long double)Math::Size(player->GetVelocity());

            s.smoothedSpeed = (s.smoothedSpeed * (1.0L - alpha)) + (speed * alpha);
            calculatedHz = s.smoothedSpeed / spacingUnits * 0.1L;

            //info("TwerkTrail [Vel] | Speed: {:.2f} | Hz: {:.2f}", (double)speed, (double)calculatedHz);
        }
        else {
            // --- Displacement Branch ---
            const long double distance = (long double)Math::Dist(currentPosition,lastPosition);

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
        if (!IsValidOf<SDK::APlayerCharacter>(Player)) return;
        if (!IsOnSpacerig()) return;
        Player->Server_CheatDancingCharacterOnSelf(18);
    }
    void TickSpam()
    {
        static int counter = 0;
        ++counter;
        auto* Player = GetLocalPlayer();
        if (!Player || !Kismet::IsValid(Player)) return;
        auto ChatStr = SDK::FString(ToWide(std::format("Tick {}", counter)).c_str());
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
        if (!IsValidOf<SDK::APlayerCharacter>(GetLocalPlayer())) return;
        SDK::TArray<SDK::AActor*> FoundActors;
        SDK::UGameplayStatics::GetAllActorsOfClass(GetWorld(), SDK::APitJaw::StaticClass(), &FoundActors);
        for (SDK::AActor* Actor : FoundActors)
            if (IsValidOf<SDK::APitJaw>(Actor)) Actor->K2_DestroyActor();
    }

    void FearAura() {
        SDK::ACoilGun* CoilGun = GetSecondaryWeapon<SDK::ACoilGun>(GetLocalPlayer());
        if (!IsValidOf<SDK::ACoilGun>(CoilGun)) return;

        SDK::TArray<SDK::AActor*> FoundActors;
        SDK::UGameplayStatics::GetAllActorsOfClass(GetWorld(), SDK::AFSDPawn::StaticClass(), &FoundActors);
        if (FoundActors.Num() == 0) return;

        if (Kismet::IsServer(GetWorld()))
        {
            for (SDK::AActor* Actor : FoundActors)
                if (IsValidOf<SDK::AActor>(Actor))
                    CoilGun->Server_FearTarget(Actor);

            return;
        }

        std::vector<SDK::AFSDPawn*> Pawns;
        Pawns.reserve(FoundActors.Num());

        for (SDK::AActor* Actor : FoundActors)
            if (SDK::AFSDPawn* Pawn = Cast<SDK::AFSDPawn>(Actor))
                if (Pawn->GetAttitude() != SDK::EPawnAttitude::Friendly)
                    Pawns.push_back(Pawn);

        EnqueueThrottled<SDK::AFSDPawn*>(std::move(Pawns), std::chrono::milliseconds(16),
            [CoilGun](SDK::AFSDPawn* Target, size_t Index, size_t Total) -> bool
            {
                if (IsValidOf<SDK::ACoilGun>(CoilGun) && IsValidOf<SDK::AFSDPawn>(Target)) CoilGun->Server_FearTarget(Target);
                return true;
            });
    }

    void CoilResistance() {
        SDK::ACoilGun* CoilGun = GetSecondaryWeapon<SDK::ACoilGun>(GetLocalPlayer());
        if (!IsValidOf<SDK::ACoilGun>(CoilGun)) return;
        CoilGun->Server_ToggleCharingBonuses(true);
    }

    void LokiAbuse()
    {
        SDK::AWPN_LockOnRifle_C* Loki = GetPrimaryWeapon<SDK::AWPN_LockOnRifle_C>(GetLocalPlayer());
        if (!IsValidOf<SDK::AWPN_LockOnRifle_C>(Loki)) return;
        Weapons::UpgradeLoki(Loki);
        Weapons::NukeEnemiesWithECR(Loki);
        //Weapons::ECRTrail(Loki, 128);
        //Weapons::LokiSphereExplosions(Loki, 1000, 1024);
    }

    void LockPlayers() {
        static const std::vector<std::string> playersToLock = { "29 Phantom" };
        auto Local = GetLocalPlayer();
        if (!GetWorld() || !GetWorld()->GetName().contains("LVL_SpaceRig")) return;
        if (!IsValidOf<SDK::APlayerCharacter>(Local)) return;
        SDK::AReplicatedActor_C* ActualTeleporter = nullptr;
        SDK::TArray<SDK::AActor*> Teleporters{};
        SDK::UGameplayStatics::GetAllActorsOfClass(GetWorld(), SDK::AReplicatedActor_C::StaticClass(), &Teleporters);
        for (auto* T : Teleporters)
            if (IsValidOf<SDK::AReplicatedActor_C>(T) && T->GetOwner() == Local)
            {
                ActualTeleporter = static_cast<SDK::AReplicatedActor_C*>(T); break;
            }
        if (!IsValidOf<SDK::AReplicatedActor_C>(ActualTeleporter)) return;
        SDK::TArray<SDK::APlayerCharacter*> Players = ActorLib::GetAllPlayerCharacters(nullptr);
        std::vector<SDK::APlayerCharacter*> Targets;
        for (auto P : Players) {
            if (IsValidOf<SDK::APlayerCharacter>(P) &&
                std::find(
                    playersToLock.begin(),
                    playersToLock.end(),
                    P->GetPlayerState()->GetPlayerName().ToString().c_str()) != playersToLock.end())
                Targets.push_back(P);
        }
        if (Targets.empty()) return;
        EnqueueThrottled<SDK::APlayerCharacter*>(Targets, std::chrono::milliseconds(20),
            [ActualTeleporter](SDK::APlayerCharacter* Target, size_t Index, size_t Total) -> bool
            {
                if (IsValidOf<SDK::APlayerCharacter>(Target) && IsValidOf<SDK::AReplicatedActor_C>(ActualTeleporter))
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
    std::vector<SDK::AAmmoDrivenWeapon*> GetAllAmmoWeapons(SDK::APlayerCharacter* LocalPlayer)
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

    void LokiSphereExplosions(SDK::AWPN_LockOnRifle_C* Loki, float R, int N)
    {
        if (N <= 0 || R <= 0) return;
        if (!IsValidOf<SDK::AWPN_LockOnRifle_C>(Loki)) return;
        std::vector<FVector> Points;
        Points.reserve(N);

        const float PHI = 3.14159265359f * (3.0f - std::sqrt(5.0f)); // golden angle
        const float Time = SDK::UKismetSystemLibrary::GetGameTimeInSeconds(GetWorld());
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

    void UpgradeLoki(SDK::AWPN_LockOnRifle_C* Loki) {
        ASSUME_ASSERT(IsValidOf<SDK::AWPN_LockOnRifle_C>(Loki));

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

    void NukeEnemiesWithECR(SDK::AWPN_LockOnRifle_C* Loki)
    {
        ASSUME_ASSERT(IsValidOf<SDK::AWPN_LockOnRifle_C>(Loki));
        float damage = 9999999;
        if (auto AoeActor = Loki->AoeActorClass) {
            auto comp = Cast<SDK::UDamageComponent>(SDK::UActorFunctionLibrary::GetComponentFromClass(Loki->AoeActorClass, SDK::UDamageComponent::StaticClass()));
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
        EnqueueThrottled<SDK::AFSDPawn*>(
            Enemies,
            std::chrono::milliseconds(100), // example throttled interval
            [Loki, damage, Scalings](SDK::AFSDPawn* Enemy, size_t index, size_t total) -> bool
            {
                if (!IsValidOf<SDK::AWPN_LockOnRifle_C>(Loki) || !IsChildOfByName(Loki, L"WPN_LockOnRifle_C"))
                {
                    warn("Loki became invalid mid run");
                    return false;
                }

                if (!IsValidOf<SDK::AFSDPawn>(Enemy) || !IsValidOf<SDK::UEnemyHealthComponent>(Enemy->GetHealthComponent())) return true;

                SDK::UEnemyHealthComponent* HealthComponent = CastChecked<SDK::UEnemyHealthComponent>(Enemy->GetHealthComponent());
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

    void ECRTrail(SDK::AWPN_LockOnRifle_C* Loki, size_t Count) {
        if (!IsValidOf<SDK::AWPN_LockOnRifle_C>(Loki) || !IsChildOfByName(Loki, L"WPN_LockOnRifle_C"))
            return;

        SDK::TArray<SDK::APlayerCharacter*> Players = SDK::UActorFunctionLibrary::GetAllPlayerCharacters(GetWorld());

        std::vector<int8> dummy(Count);

        EnqueueThrottled<int8>(
            dummy,
            std::chrono::milliseconds(100),
            [Loki](int8, size_t index, size_t total) -> bool {
                if (!IsValidOf<SDK::AWPN_LockOnRifle_C>(Loki) || !IsChildOfByName(Loki, L"WPN_LockOnRifle_C"))
                    return false;

                auto Players = SDK::UActorFunctionLibrary::GetAllPlayerCharacters(GetWorld());

                for (auto Player : Players) {
                    if (!IsValidOf<SDK::APlayerCharacter>(Player) || !IsChildOfByName(Player, L"PlayerCharacter"))
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
    // BuildExplicitCallName / BuildFuncSig / ScanFunctions live in
    // SharedLib/Lib_Scan.cpp — same namespace, same signatures, no shim needed.
    // Only DRG-specific helpers (DoScan with player-ownership filter +
    // State::ScannedFunctions cache) stay in this TU.

    void DoScan()
    {
        // Clear up-front so any early return leaves an empty (not stale) cache —
        // stale UObject*/UFunction* from a previous world are a crash risk.
        State::ScannedFunctions.clear();
        State::ScannedFunctionVariantsByName.clear();

        SDK::UWorld* world = GetWorld();
        if (!world) { warn("[scan] No world (transitioning?) — skipping scan."); return; }

        SDK::APlayerCharacter* Local = GetLocalPlayer();
        if (!Local) { warn("[scan] No local player."); return; }
        SDK::TArray<SDK::AActor*> AllActors;
        SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::AActor::StaticClass(), &AllActors);
        int inserted = 0;
        auto TryScanObject = [&](SDK::UObject* Obj)
            {
                for (SDK::UClass* cls : UClassHierarchyRange(Obj->Class))
                    for (auto* field : SDK::UFieldRange(cls->Children))
                    {
                        if (!field->IsA(SDK::UFunction::StaticClass())) continue;
                        auto* Func = static_cast<SDK::UFunction*>(field);
                        auto  flags = static_cast<SDK::EFunctionFlags>(Func->FunctionFlags);
                        if (!(flags & SDK::EFunctionFlags::Net))       continue;
                        if (!(flags & SDK::EFunctionFlags::NetServer)) continue;
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
            if (NoneOf<SDK::AActor*>(Actor->GetOwner(), Local, Local->GetOwner(),
                Local->GetPlayerController(), Local->GetPlayerState())) continue;
            TryScanObject(Actor);
            SDK::TArray<SDK::UActorComponent*> Components =
                Actor->K2_GetComponentsByClass(SDK::UActorComponent::StaticClass());
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
    void DanceIntercept(SDK::UObject* Object, SDK::UFunction*, void* Params)
    {
        auto* Args = static_cast<SDK::Params::PlayerCharacter_Server_SetIsDancing*>(Params);

        auto HandlePlayer = [&](SDK::APlayerCharacter* Local)
            {
                if (Object == Local) return;
                if (Args->danceMove_0 != 18) return;
                auto* Player = static_cast<SDK::APlayerCharacter*>(Object);

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
                        SDK::FString(L"Six Seven"), SDK::EChatSenderType::DeluxUser);
                    Args->danceMove_0 = -1;
                    Args->isDancing_0 = false;
                }
            };

        if constexpr (CheckLocal) HandlePlayer(GetLocalPlayer());
        else                      HandlePlayer(GetLocalPlayerCharacterBlocking(5));
    }

    void ServerMessageIntercept(SDK::UObject*, SDK::UFunction*, void* Params)
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

    void MessageIntercept(SDK::UObject*, SDK::UFunction*, void* Params)
    {
        auto* msg = static_cast<SDK::Params::FSDPlayerController_Server_NewMessage*>(Params);
        if (msg->Text.ToString() == "Six Seven") return;
        info("[Chat] {}: {}", msg->Sender.ToString(), msg->Text.ToString());
    }

    void ProxyModHook(SDK::UObject*, SDK::UFunction*, void*)
    {
        SDK::UClass* ProxyMod = SDK::BasicFilesImpleUtils::FindClassByName("ProxyMod_C", false);
        if (!ProxyMod || !Kismet::IsValidClass(ProxyMod)) { error("[ProxyMod] Class not found."); return; }
        SDK::UFunction* InitFunc = ProxyMod->GetFunction("ProxyMod_C", "Init");
        if (!InitFunc || !Kismet::IsValid(InitFunc)) { error("[ProxyMod] Function 'Init' missing."); return; }
        auto* ProxyActor = GetActorOfClass<SDK::AActor>(ProxyMod);
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

    void MakeJSON(const CommandContext& JS)
    {}

    //void tpbars(const ctx&){
    //    if (!IsValidOf<SDK::APlayerCharacter>(GetLocalPlayer())) return;
    //    std::vector<SDK::AActor*> Relevant{};
    //    SDK::TArray<SDK::AActor*> Barnacles = GetAllActorsOfClass<SDK::ABhaBarnacle>();
    //    SDK::TArray<SDK::AActor*> Apocas = GetAllActorsOfClass<>
    //    SDK::TArray<SDK::AActor*> Teleports = GetAllActorsOfClass<SDK::AReplicatedActor_C>();
    //    for(auto T : Teleports) {
    //        if(!IsValidOf<SDK::AReplicatedActor_C>(T)) continue;
    //        if(T->GetOwner() != GetLocalPlayer()) continue;
    //        auto TP = CastChecked<SDK::AReplicatedActor_C>(T);
    //        for(auto A : FoundActors) {
    //            if(!IsValidOf<SDK::ABhaBarnacle>(A)) continue;
    //            TP->Server_TeleportPlayer()
    //
    //        }
    //    }
    //}
    //void Resup(const ctx&) {
    //    if (!IsValidOf<SDK::APlayerCharacter>(GetLocalPlayer())) return;
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
        if (!IsValidOf<SDK::APlayerCharacter>(GetLocalPlayer())) return;
        SDK::TArray<SDK::AActor*> FoundActors = GetAllActorsOfClass<SDK::AActor>();

        std::vector<SDK::AActor*> Filter;
        Filter.reserve(FoundActors.Num());

        for (auto A : FoundActors) {
            if (!IsValidOf<SDK::AActor>(A)) continue;
            if (!A->bReplicates) continue;
            if (NoneOf<SDK::AActor*>(A->GetOwner(), GetLocalPlayer(), GetLocalPlayer()->GetOwner(),
                GetLocalPlayer()->GetPlayerController(), GetLocalPlayer()->GetPlayerState())) continue;

            Filter.push_back(A);
        }

        std::erase_if(Filter, [](SDK::AActor* A) {
            if (!IsValidOf<SDK::AActor>(A)) return true;
            if (!A->Class) return true;

            auto ClassName = A->Class->Name.ToString();
            return !ClassName.contains("_DamageList_");
            });
        info("Found {} potential damage meter actors.", Filter.size());
        for (auto A : Filter) {
            info("Potential actor: {}, class: {}", A->Name.ToString(), A->Class->Name.ToString());
        }
    }

    static void Untwerk(SDK::APlayerCharacter* Player)
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
        Local->Server_SpawnEnemies(SDK::UEnemyDescriptor::GetDefaultObj(), 100000);
        info("Crash triggered.");
    }
    void Say(const CommandContext& ctx)
    {
        SDK::UFSDGameInstance* Instance = GameLib::GetFSDGameInstance(GetWorld());
        if (!IsValidOf<SDK::UFSDGameInstance>(Instance)) return;
        SDK::AFSDPlayerState* LocalState = Instance->GetLocalFSDPlayerController()->GetFSDPlayerState();
        if (!IsValidOf<SDK::AFSDPlayerState>(LocalState)) return;
        const std::string& SenderName = ctx.Arg(1);
        SDK::AFSDPlayerState* Sender = nullptr;
        for (SDK::AFSDPlayerState* Player : SDK::UGameFunctionLibrary::GetFSDGameState(GetWorld())->GetNetworkSortedPlayerArray()) {
            if (!IsValidOf<SDK::AFSDPlayerState>(Player)) continue;
            if (Player->GetPlayerName().ToString() == SenderName) {
                Sender = Player;
                break;
            }
        }
        // If no player matched, arg 1 is part of the message, not a name
        const auto msgBegin = IsValidOf<SDK::AFSDPlayerState>(Sender)
            ? ctx.args.begin() + 2
            : ctx.args.begin() + 1;

        if (!IsValidOf<SDK::AFSDPlayerState>(Sender)) Sender = LocalState;
        std::string msg;

        for (auto it = msgBegin; it != ctx.args.end(); ++it) { if (it != msgBegin) msg += ' '; msg += *it; }

        LocalState->GetPlayerController()->Server_NewMessage(
            Sender->GetPlayerName(),
            SDK::FString(ToWide(msg).c_str()),
            Sender->GetChatSenderType()
        );
    }

    void CoilCarve(const CommandContext& ctx)
    {
        auto* L = GetLocalPlayer();
        if (!L || !Kismet::IsValid(L)) return;
        auto* WPN = L->InventoryComponent->GetItem(SDK::EItemCategory::SecondaryWeapon);
        if (!WPN || !Kismet::IsValid(WPN) || !WPN->IsA(SDK::ACoilGun::StaticClass())) return;
        auto* CoilGun = CastChecked<SDK::ACoilGun>(WPN);
        SDK::FVector_NetQuantize Start{}, End{};
        Start.X = SafeStof(ctx.Arg(1)); Start.Y = SafeStof(ctx.Arg(2)); Start.Z = SafeStof(ctx.Arg(3));
        End.X = SafeStof(ctx.Arg(4)); End.Y = SafeStof(ctx.Arg(5)); End.Z = SafeStof(ctx.Arg(6));
        CoilGun->Server_HitTerrain(Start, End, SafeStof(ctx.Arg(7)));
    }

    void AutoCrasher(const CommandContext& = State::dummyCtx)
    {
        State::autoCrasherEnabled = !State::autoCrasherEnabled;
        if(State::autoCrasherEnabled) 
        {
            EnqueueWhile(
                []() -> bool 
                {
                    if (!State::autoCrasherEnabled) return false;
                    SDK::UWorld* World = GetWorld();
                    if (!World) return true;
                    if (Kismet::IsServer(World)) return true;
                    SDK::APlayerCharacter* Player = GetLocalPlayer();
                    if (!IsValidOf<SDK::APlayerCharacter>(Player)) return true;
                    Player->Server_SpawnEnemies(SDK::UEnemyDescriptor::GetDefaultObj(), 100000);
                    info("AutoCrasher triggered");
                    return true;
                }
            );
            info("Autocrasher enabled.");
        }
        else 
        {
            info("Autocrasher disabled.");
        }
            
    }

    void FearAll(const CommandContext& = State::dummyCtx)
    {
        SDK::APlayerCharacter* Player = GetLocalPlayer();
        if (!Kismet::IsValid(Player)) return;

        SDK::ACoilGun* CoilGun = Cast<SDK::ACoilGun>(Player->InventoryComponent->GetItem(SDK::EItemCategory::SecondaryWeapon));
        if (!Kismet::IsValid(CoilGun)) return;

        SDK::TArray<SDK::AActor*> FoundActors;
        SDK::UGameplayStatics::GetAllActorsOfClass(GetWorld(), SDK::AFSDPawn::StaticClass(), &FoundActors);

        if (FoundActors.Num() == 0) return;

        const bool bIsServer = Kismet::IsServer(GetWorld());

        if (bIsServer)
        {
            for (SDK::AActor* Actor : FoundActors)
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
            std::vector<SDK::AFSDPawn*> Pawns;
            Pawns.reserve(FoundActors.Num());

            for (SDK::AActor* Actor : FoundActors)
            {
                if (SDK::AFSDPawn* Pawn = Cast<SDK::AFSDPawn>(Actor))
                {
                    Pawns.push_back(Pawn);
                }
            }

            EnqueueThrottled<SDK::AFSDPawn*>(std::move(Pawns), std::chrono::milliseconds(16),
                [CoilGun](SDK::AFSDPawn* Target, size_t Index, size_t Total) -> bool
                {
                    if (Kismet::IsValid(CoilGun) && Kismet::IsValid(Target))
                    {
                        CoilGun->Server_FearTarget(Target);
                    }
                    return true;
                });
        }
    }

    static GameHooks::HookToggle<GameHooks::ProcessEventHook> s_chatLog{
        [] { return GameHooks::OnProcessEventByNameAndClass(
            "ClientNewMessage", SDK::AFSDGameState::StaticClass(), Callbacks::ServerMessageIntercept); } };

    void LogChat(const CommandContext& = State::dummyCtx)
    {
        info("[cmd:logchat] Chat logging {}", s_chatLog.Toggle() ? "enabled" : "disabled");
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
                Curr->PlayerState->GetPlayerName(), SDK::FString(L"Six Seven"),
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

    static GameHooks::HookToggle<GameHooks::ProcessEventHook> s_ignoreProxy{
        []() -> GameHooks::CallbackHandle
        {
            SDK::UClass* ProxyMod = SDK::BasicFilesImpleUtils::FindClassByName("ProxyMod_C", false);
            if (!ProxyMod || !Kismet::IsValidClass(ProxyMod))
            {
                error("[cmd:ignoreproxy] Failed to find class 'ProxyMod_C'.");
                return 0;
            }
            return GameHooks::OnProcessEventByNameAndClass("Init", ProxyMod, Callbacks::ProxyModHook);
        } };

    void IgnoreProxy(const CommandContext& = State::dummyCtx)
    {
        info("[cmd:ignoreproxy] ProxyMod hook {}", s_ignoreProxy.Toggle() ? "enabled" : "disabled");
    }

    void Prop(const CommandContext& ctx) { PropertyInspector::DispatchCommand(ctx); }

    void ScanReplicated(const CommandContext& ctx)
    {
        SDK::APlayerCharacter* Local = GetLocalPlayer();
        if (!Local) { warn("[cmd:scan] No local player."); return; }
        SDK::TArray<SDK::AActor*> AllActors;
        SDK::UGameplayStatics::GetAllActorsOfClass(GetWorld(), SDK::AActor::StaticClass(), &AllActors);
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
            if (NoneOf<SDK::AActor*>(Actor->GetOwner(), Local, Local->GetOwner(),
                Local->GetPlayerController(), Local->GetPlayerState())) continue;
            std::vector<std::string> actorFuncs;
            Scan::ScanFunctions(Actor, actorFuncs);
            SDK::TArray<SDK::UActorComponent*> Components =
                Actor->K2_GetComponentsByClass(SDK::UActorComponent::StaticClass());
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
        // Rescan only on a cache miss. A world change clears the cache via
        // OnWorldChanged(), so the next call misses and rebinds live owners; the
        // IsUsable gate below catches any residual staleness before ProcessEvent.
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

                // A cached target is only safe to call if BOTH the owner object and
                // the UFunction are still live. Raw pointers can dangle after a world
                // change (and the freed slot may be reused), so validate before use.
                auto IsUsable = [](State::ScannedFunction* c) -> bool
                {
                    return c && c->Owner && IsValidRaw(c->Owner) && IsValid(c->Owner)
                        && IsInActiveWorld(c->Owner) && IsValidOf<SDK::UFunction>(c->Func);
                };

                State::ScannedFunction* candidate = ResolveUniqueVariant(funcName);
                if (!candidate)
                    candidate = ResolveByBaseName(funcName);
                if (!candidate) return nullptr;

                if (!IsUsable(candidate))
                {
                    // Copy lookup keys before DoScan() clears/rebuilds the map and
                    // invalidates `candidate` (a pointer into it).
                    const std::string explicitName = candidate->ExplicitName;
                    const std::string functionName = candidate->FunctionName;
                    warn("[cmd:call] cached target stale for '{}' (world change?); rescanning.", funcName);
                    Scan::DoScan();
                    candidate = ResolveUniqueVariant(explicitName);
                    if (!candidate)
                        candidate = ResolveByBaseName(functionName);
                }

                // Final gate: never hand back a target we cannot safely ProcessEvent on.
                if (!IsUsable(candidate))
                {
                    warn("[cmd:call] '{}' could not be resolved to a live object; aborting.", funcName);
                    return nullptr;
                }

                return candidate;
            };

        State::ScannedFunction* target = ResolveCachedCallTarget();
        if (!target) { warn("[cmd:call] '{}' not found.", funcName); return; }

        auto* Func = target->Func;
        auto* Owner = target->Owner;
        std::vector<SDK::FProperty*> parmProps;
        for (SDK::FField* field : SDK::FFieldRange(Func->ChildProperties))
        {
            if (!SDK::FieldCast::IsA<SDK::FProperty>(field)) continue;
            SDK::FProperty* Prop = static_cast<SDK::FProperty*>(field);
            SDK::EPropertyFlags  pf = static_cast<SDK::EPropertyFlags>(Prop->PropertyFlags);
            if (!(pf & SDK::EPropertyFlags::Parm))    continue;
            if (pf & SDK::EPropertyFlags::ReturnParm) continue;
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
                SDK::FieldCast::Visit(parmProps[i], [&]<typename T>(T * p)
                {
                    if constexpr (std::is_same_v<T, SDK::FObjectProperty> ||
                        std::is_same_v<T, SDK::FObjectPropertyBase> ||
                        std::is_same_v<T, SDK::FClassProperty> ||
                        std::is_same_v<T, SDK::FWeakObjectProperty>)
                        *GetPropertyPtr<SDK::UObject*>(base, p->Offset) = expanded.object;
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
        if (static_cast<SDK::EFunctionFlags>(Func->FunctionFlags) & SDK::EFunctionFlags::Native) Func->FunctionFlags |= 0x400;
        Owner->ProcessEvent(Func, parmsSize > 0 ? parmsBuf.data() : nullptr);
        Func->FunctionFlags = savedFlags;
        for (SDK::FProperty* Prop : parmProps)
            SDK::FieldCast::Visit(Prop, [&]<typename T>(T * p)
        {
            using namespace SDK;
            uintptr_t base = reinterpret_cast<uintptr_t>(parmsBuf.data());
            if constexpr (std::is_same_v<T, SDK::FStrProperty>)
                GetPropertyPtr<FString>(base, p->Offset)->~FString();
            else if constexpr (std::is_same_v<T, SDK::FNameProperty>)
                GetPropertyPtr<FName>(base, p->Offset)->~FName();
        });
        info("[cmd:call] Called '{}' on '{}'.", funcName, Owner->GetName());
    }

    void TeleportAllToSelf(const ctx& = State::dummyCtx) {
        if (!IsValidOf<SDK::APlayerCharacter>(GetLocalPlayer())) return;
        SDK::TArray<SDK::APlayerCharacter*> Players = ActorLib::GetAllPlayerCharacters(GetWorld());
        FVector Coords = GetLocalPlayer()->K2_GetActorLocation();
        for (auto TargetPlayer : Players)
            if (Kismet::IsServer(GetWorld()))
            {
                SDK::FHitResult Hit;
                if (IsValidOf<SDK::APlayerCharacter>(TargetPlayer))
                    TargetPlayer->K2_SetActorLocation(Coords, false, &Hit, true);
            }
            else
            {
                for (auto* Actor : GetAllActorsOfClass<SDK::AReplicatedActor_C>())
                    static_cast<SDK::AReplicatedActor_C*>(Actor)->Server_TeleportPlayer(TargetPlayer, Coords);
            }
    }

    void PlayerTeleport(const ctx& ctx) {
        if (!IsValidOf<SDK::APlayerCharacter>(GetLocalPlayer())) return;

        SDK::TArray<SDK::APlayerCharacter*> Players = ActorLib::GetAllPlayerCharacters(GetWorld());
        if (Players.Num() == 0) return;

        MutableContext mutableCtx = MutableContext(ctx);
        FVector Coords{};
        std::vector<std::pair<std::string, SDK::APlayerCharacter*>> NameToPlayer{};
        bool    foundDestinationPlayer = false;
        bool    targetIsSelf = false;
        bool    foundPlayerToMove = false;
        NameToPlayer.reserve(Players.Num());

        for (SDK::APlayerCharacter* Player : Players) {
            if (!IsValidOf<SDK::APlayerCharacter>(Player)) continue;
            std::pair<std::string, SDK::APlayerCharacter*> entry{ Player->PlayerState->GetPlayerName().ToString(), Player };
            NameToPlayer.push_back(entry);
        }

        std::erase(mutableCtx.args, "ptp");

        SDK::APlayerCharacter* TargetPlayer = nullptr;
        SDK::APlayerCharacter* DestPlayer = nullptr;

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
            SDK::FHitResult Hit;
            if (IsValidOf<SDK::APlayerCharacter>(TargetPlayer))
                TargetPlayer->K2_SetActorLocation(Coords, false, &Hit, true);
        }
        else
        {
            for (auto* Actor : GetAllActorsOfClass<SDK::AReplicatedActor_C>())
                static_cast<SDK::AReplicatedActor_C*>(Actor)->Server_TeleportPlayer(TargetPlayer, Coords);
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
        FVector Dir = { SafeStof(mutableCtx.Arg(0)),
                        SafeStof(mutableCtx.Arg(1)),
                        SafeStof(mutableCtx.Arg(2)) };
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
            SDK::FHitResult Hit; Local->K2_SetActorLocation(Coords, sweep, &Hit, true);
        }
        else
        {
            SDK::TArray<SDK::AActor*> Actors;
            SDK::UGameplayStatics::GetAllActorsOfClass(GetWorld(), SDK::AReplicatedActor_C::StaticClass(), &Actors);
            for (auto* Actor : Actors)
                static_cast<SDK::AReplicatedActor_C*>(Actor)->Server_TeleportPlayer(Local, Coords);
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
            SDK::FHitResult Hit; Local->K2_SetActorLocation({ 0, 0, 12550 }, false, &Hit, true);
        }
        else
        {
            SDK::TArray<SDK::AActor*> Actors;
            SDK::UGameplayStatics::GetAllActorsOfClass(GetWorld(), SDK::AReplicatedActor_C::StaticClass(), &Actors);
            for (auto* Actor : Actors)
                static_cast<SDK::AReplicatedActor_C*>(Actor)->Server_TeleportPlayer(Local, { 0, 0, 12550 });
        }
    }

    void Troll(const CommandContext& = State::dummyCtx)
    {
        SDK::APlayerCharacter* Local = GetLocalPlayer();
        SDK::AReplicatedActor_C* ActualTeleporter = nullptr;
        SDK::TArray<SDK::AActor*> AllActors, Teleporters;
        SDK::UGameplayStatics::GetAllActorsOfClass(GetWorld(), SDK::AActor::StaticClass(), &AllActors);
        SDK::UGameplayStatics::GetAllActorsOfClass(GetWorld(), SDK::AReplicatedActor_C::StaticClass(), &Teleporters);
        for (auto* T : Teleporters)
            if (T && Kismet::IsValid(T) && T->GetOwner() == Local)
            {
                ActualTeleporter = static_cast<SDK::AReplicatedActor_C*>(T); break;
            }
        if (!ActualTeleporter) { warn("[cmd:troll] No teleporter found."); return; }
        std::vector<SDK::AActor*> actors;
        for (auto* A : AllActors) if (A && Kismet::IsValid(A) && A->bReplicates) actors.push_back(A);
        info("[cmd:troll] Found {} actors, starting teleport loop.", actors.size());
        auto rng = std::make_shared<std::mt19937>(std::random_device{}());
        auto teleporter = ActualTeleporter;
        EnqueueThrottled<SDK::AActor*>(std::move(actors), std::chrono::milliseconds(16),
            [rng, teleporter](SDK::AActor* target, size_t i, size_t total) -> bool
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
        SDK::FString Descriptor = ToFString(ctx.Arg(1));
        int     Count = std::stoi(ctx.Arg(2));
        FVector Location = { std::strtof(ctx.Arg(3).c_str(), nullptr),
                                std::strtof(ctx.Arg(4).c_str(), nullptr),
                                std::strtof(ctx.Arg(5).c_str(), nullptr) };
        SDK::TArray<SDK::AActor*> Actors;
        SDK::UGameplayStatics::GetAllActorsOfClass(GetWorld(), SDK::AReplicatedActor_C::StaticClass(), &Actors);
        for (auto* Actor : Actors)
            static_cast<SDK::AReplicatedActor_C*>(Actor)->Server_SpawnEnemy(Descriptor, Count, Location, false, false);
        info("[cmd:spawnenemies] Spawned {} '{}' at ({}, {}, {})",
            Count, ctx.Arg(1), Location.X, Location.Y, Location.Z);
    }

    void SpawnAndDump(const CommandContext& ctx)
    {
        if (!Kismet::IsServer(GetWorld())) return;
        const long double delayMs = static_cast<long double>(std::max(200.0f, SafeStof(ctx.Arg(1))));

        //Request all descriptors and all biomes. iterate on all of them including elite versions. Only spawn enemy ONCE , keep track of UClasses.
        std::vector<SDK::UEnemyDescriptor*> descriptors{};
        descriptors.reserve(512);
        for (auto* Descriptor : GObjectsOf<SDK::UEnemyDescriptor>())
            if (Descriptor->GetName().starts_with("ED_")) descriptors.push_back(Descriptor);
        std::vector<SDK::UBiome*> biomes{};
        for (auto* Biome : GObjectsOf<SDK::UBiome>())
            biomes.push_back(Biome);

        // Build class → combos map: for each descriptor × (null + all biomes) × (non-elite + elite)
        // call GetEnemyClass; group all (desc,biome,isElite) tuples that resolve to the same SDK::UClass.
        struct Combo { std::string desc, biome; bool isElite; };
        struct Entry { SDK::UClass* cls = nullptr; std::vector<Combo> combos; };
        std::unordered_map<SDK::UClass*, Entry> classMap;
        classMap.reserve(256);

        for (auto* Desc : descriptors)
        {
            auto tryAdd = [&](SDK::UBiome* Biome, bool IsElite)
                {
                    SDK::UClass* cls = static_cast<SDK::UClass*>(Desc->GetEnemyClass(Biome, IsElite));
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

        SDK::APlayerCharacter* Player = GetLocalPlayer();
        if (!IsValidOf<SDK::APlayerCharacter>(Player)) { warn("[spawndump] No local player"); return; }
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
        auto dumpNumeric = [](SDK::UObject* Obj, SDK::UClass* StopAt) -> nlohmann::json
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

        auto prevPawn = std::make_shared<SDK::APawn*>(nullptr);
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
                    auto actorProps = ExtractNumericPropertiesAsJson(reinterpret_cast<uintptr_t>(*prevPawn), SDK::AActor::StaticClass());
                    for (auto& [key, value] : actorProps.items())
                        enemyEntry[key] = value;

                    // Add component properties with component name prefix
                    auto comps = (*prevPawn)->K2_GetComponentsByClass(SDK::UActorComponent::StaticClass());
                    for (auto* comp : comps)
                    {
                        if (comp->Outer == *prevPawn)
                        {
                            auto compProps = ExtractNumericPropertiesAsJson(
                                reinterpret_cast<uintptr_t>(comp),
                                SDK::UActorComponent::StaticClass(),
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

                SDK::FTransform SpawnTransform{};
                SpawnTransform.Translation = SpawnLoc;
                SDK::APawn* Pawn = nullptr;
                SpawnActor<SDK::APawn>(current->cls, SpawnTransform, Pawn);
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

        std::vector<SDK::UEnemyDescriptor*> descriptors;
        descriptors.reserve(512);
        for (SDK::UEnemyDescriptor* Descriptor : GObjectsOf<SDK::UEnemyDescriptor>())
            if (Descriptor->GetName().starts_with("ED_"))
                descriptors.push_back(Descriptor);

        struct Entry { SDK::UClass* cls = nullptr; std::string desc; };

        std::unordered_map<SDK::UClass*, Entry> classMap;
        classMap.reserve(256);
        for (auto* Desc : descriptors)
        {
            SDK::UClass* cls = static_cast<SDK::UClass*>(Desc->GetEnemyClass(nullptr, false));
            if (!IsValidClass(cls)) continue;
            classMap.emplace(cls, Entry{ cls, Desc->GetName() });
        }

        if (classMap.empty()) { warn("[spawndump] No enemy classes resolved"); return; }
        info("[spawndump] {} unique classes from {} descriptors, delay={:.0f}ms",
            classMap.size(), descriptors.size(), static_cast<double>(delayMs));

        SDK::APlayerCharacter* Player = GetLocalPlayer();
        if (!IsValidOf<SDK::APlayerCharacter>(Player)) { warn("[spawndump] No local player"); return; }

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

        auto prevPawn  = std::make_shared<SDK::APawn*>(nullptr);
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
                    SDK::TArray<SDK::UActorComponent*> comps =
                        (*prevPawn)->K2_GetComponentsByClass(SDK::UPrimitiveComponent::StaticClass());
                    for (SDK::UActorComponent* actorComp : comps)
                    {
                        auto* comp = Cast<SDK::UPrimitiveComponent>(actorComp);
                        if (!comp) continue;
                        for (uint32 i = 0, n = comp->GetNumMaterials(); i < n; ++i)
                        {
                            SDK::UMaterialInterface* mat = comp->GetMaterial(i);
                            while (auto* mid = Cast<SDK::UMaterialInstanceDynamic>(mat))
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
                static SDK::FTransform SpawnTransform{};
                SpawnTransform.Translation = SpawnLoc;

                SDK::APawn* Pawn = nullptr;
                SpawnActor<SDK::APawn>(current->cls, SpawnTransform, Pawn);
                *prevPawn  = Pawn;
                *prevEntry = Pawn ? current : nullptr;
                if (!Pawn)
                    warn("[spawndump] ({}/{}) Spawn failed: {}", idx, total - 1, current->cls->GetName());
                return true;
            });
    }

    void DumpDescriptors(const CommandContext& = State::dummyCtx)
    {
        // SDK::UEnum* for a named field via SDK::FEnumProperty / SDK::FByteProperty reflection
        auto getFieldEnum = [](SDK::UClass* Cls, const char* Name) -> SDK::UEnum*
            {
                SDK::FName needle(ToWide(std::string(Name)).c_str());
                for (SDK::FField* field : SDK::FFieldRange(Cls->ChildProperties))
                {
                    if (field->Name != needle) continue;
                    if (auto* ep = SDK::FieldCast::Cast<SDK::FEnumProperty>(field)) return ep->Enum;
                    if (auto* bp = SDK::FieldCast::Cast<SDK::FByteProperty>(field))  return bp->Enum;
                }
                return nullptr;
            };

        // Enum integer → short name via SDK::UEnum::Names, strips "EnumClass::" prefix
        auto enumName = [](SDK::UEnum* E, int64 Val) -> std::string
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

        // One SDK::FField walk per enum type, not per descriptor
        SDK::UClass* DescClass = SDK::UEnemyDescriptor::StaticClass();
        SDK::UEnum* SigEnum = getFieldEnum(DescClass, "EnemySignificance");
        SDK::UEnum* VetEnum = getFieldEnum(DescClass, "VeteranScaling");

        nlohmann::json root = nlohmann::json::object();

        for (auto* Desc : GObjectsOf<SDK::UEnemyDescriptor>())
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
            d["UsesVeteranLarge"] = (Desc->VeteranScaling == SDK::EVeteranScaling::LargeEnemy);

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

    void InfiniteAmmo() {
        Commands::ToggleInfiniteAmmo();
    }

    void RegisterKeybinds() {
        using enum Key;
        using enum Trigger;
        using enum Focus;

        KeyBindings::RegisterGameThread(
            Key::F4, Mod::None, Binds::Crash,
            BindingOptions{ Press, Game, true });

        KeyBindings::RegisterGameThread(
            I, Mod::Ctrl, Binds::InfiniteAmmo,
            BindingOptions{ Press, Game, true });

        // Aimbot, RCS, and Silent Aim keybinds live in Aim.
        AimAssist::RegisterKeybinds();
    }
}

// =========================================================================
// Public interface
// =========================================================================

void RegisterCommands(CommandHandler& handler)
{
    using namespace VarSystem;

    // Shared, game-agnostic commands: lognet*, reloadnetlog, exec, scanall, findclass,
    // logallevents, stoptick, get/set/unset/vars.
    RegisterSharedCommands(handler);

    // Chat
    handler.Register("logchat", Commands::LogChat, "Chat", R"(Toggle chat message logging)");
    handler.Register("say", Commands::Say, "Chat", R"(Send message to chat: say [sender] <message>)");

    // Enemies
    handler.Register("dumpdescriptors", Commands::DumpDescriptors, "Enemies", R"(Dump all ED_* descriptor controls to C:\Dumper-7\DescriptorDump.json)");
    handler.Register("fearall", Commands::FearAll, "Enemies", R"(Instantly fear all enemies (requires Coil Gun equipped))");
    handler.Register("spawndump", Commands::SpawnAndDump, "Enemies", R"(Dump float/bool/int props for all enemy descriptors x biomes: spawndump [delay_ms])");
    handler.Register("spawnenemies", Commands::SpawnEnemies, "Enemies", R"(Spawn enemies: spawnenemies <descriptor> <count> <x> <y> <z>)");
    handler.Register("spawnmatdump", Commands::SpawnAndDumpMaterials, "Enemies", R"(Dump material names for all enemy descriptors x biomes: spawnmatdump [delay_ms])");

    // Inspection
    handler.Register("inventory", Commands::Inventory, "Inspection", R"(Dump local player inventory)");
    handler.Register("prop", Commands::Prop, "Inspection", R"(prop <cdo|obj> <name> <dump|get|set|list> [...])");
    handler.Register("scandmg", Commands::ScanDamageMeeterMod, "Inspection", R"(Scan damage meeter mod for usable actors)");
    handler.Register("scanfuncs", Commands::ScanReplicated, "Inspection", R"(Scan for usable replicated functions)");

    // Player
    handler.Register("coilcarve", Commands::CoilCarve, "Player", R"(Carve terrain with the Coil Gun: coilcarve <x1> <y1> <z1> <x2> <y2> <z2> <power>)");
    handler.Register("godmode", Commands::ToggleGodmode, "Player", R"(Toggle god mode for local player)");
    handler.Register("infiniteammo", Commands::ToggleInfiniteAmmo, "Player", R"(Toggle infinite ammo for local player)");
    handler.Register("readname", Commands::ReadName, "Player", R"(Read the name of the local player)");
    handler.Register("rename", Commands::Rename, "Player", R"(Rename the local player: rename <new_name>)");

    // Aimbot + RCS + Silent Aim + wpinfo all live in Lib_AimAssist.
    AimAssist::RegisterCommands(handler);

    // System
    handler.Register("call",          Commands::Call,          "System", R"(Call a server RPC: call <FunctionName> [args...])");
    handler.Register("clearlog", [](const CommandContext&) {
        // closes the file_sink's handle, reopens in
        // truncate mode under the sink mutex. Returns false if the reopen
        // failed (typically: external editor holds an exclusive lock).
        extern bool TruncateLogFile();
        if (TruncateLogFile())
            spdlog::info("[clearlog] file log truncated");
        else
            spdlog::warn("[clearlog] truncate failed — file probably locked by another process");
    }, "System", R"(Truncate the on-disk file log (next to the DLL))");
    handler.Register("ignoreproxy", Commands::IgnoreProxy, "System", R"(Toggle ProxyMod hook)");
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
    handler.Register("autocrasher", Commands::AutoCrasher, "Troll", R"(Toggle auto-crash)");

    // Keybindings
    KeyBindings::RegisterCommands(handler);
    Binds::RegisterKeybinds();
    
}

void SendCommandList(const CommandContext& ctx, const CommandHandler& handler)
{
    if (!g_pRespBuffer || !g_hRespEvent) return;

    constexpr DWORD MAX_WAIT_MS = 1000;
    ULONGLONG deadline = GetTickCount64() + MAX_WAIT_MS;
    while (g_pRespBuffer->ready.load(std::memory_order_acquire))
    {
        if (GetTickCount64() > deadline) return;
        Sleep(5);
    }

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
    InitSharedCallbacks();
    Commands::LogChat();
    Commands::Twerk();
    JsonHook::Setup();
    AimAssist::ToggleSilentAim();

    TickSystem::SetTickableFunction_AsIntervalMs(Tickables::DeletePitjaws, 5000L);
    TickSystem::SetTickableFunction_AsFrequencyHz(Tickables::LockPlayers, 1L);
    //TickSystem::SetTickableFunction_AsFrequencyHz(Tickables::SpawnTwerk,Twerking::GetSpawnTwerkTrailFrequencyHz);
    //TickSystem::SetTickableFunction_AsFrequencyHz(Tickables::FearAura, 2L);
    //TickSystem::SetTickableFunction_AsIntervalMs(Tickables::LokiAbuse, 2500L);
}

void ResetCallbackHandles()
{
    // HookToggle callbacks reset centrally via GameHooks::ResetAllToggles()
    // (called from ModManager::UnloadMods). DanceCallback is still a manual handle.
    State::DanceCallback = 0;
}

// =========================================================================
// Game policy hooks (called by the shared ModManager)
// =========================================================================

// GMalloc sanity check — verifies UnrealAllocator is reachable and functional
// before we install hooks. Moved here from ModManager (DRG-specific).
static bool TestAllocator()
{
    constexpr uint32 size = 1024;
    constexpr const char* testStr = "Hello from GMalloc!\0";

    SDK::UnrealAllocator* allocator = SDK::UnrealAllocator::Get(true);
    if (!allocator)
    {
        error("[ModManager] UnrealAllocator is null");
        return false;
    }
    info("[ModManager] Found GMalloc at {:p} ({})",
        static_cast<const void*>(allocator),
        FString(allocator->GetDescriptiveName()).ToString());
    info("[ModManager] Testing GMalloc...");

    void* mem = SDK::FMemory::Malloc(size);
    if (!mem)
    {
        error("[ModManager] GMalloc allocation failed.");
        return false;
    }

    SDK::FMemory::Memcpy(mem, testStr, 21);

    uint64 actualSize = 0;
    if (!SDK::FMemory::GetAllocSize(mem, actualSize) || actualSize < size)
    {
        error("[ModManager] Allocation size validation failed (got {}).", actualSize);
        SDK::FMemory::Free(mem);
        return false;
    }

    info("[ModManager] GMalloc allocation successful.");
    info("[ModManager] Allocated size: {} bytes", actualSize);
    info("[ModManager] Test allocation address: {:p}", mem);
    info("[ModManager] Is Allocator Internally Thread Safe? {}", allocator->IsInternallyThreadSafe() ? "Yes" : "No");
    info("[ModManager] Test allocation content: {}", std::string(static_cast<char*>(mem), 21));
    info("[ModManager] Test allocation content validity: {}", std::string(static_cast<char*>(mem)) == testStr ? "Valid" : "Invalid");
    info("[ModManager] GMalloc test passed.");
    SDK::FMemory::Free(mem);
    return true;
}

bool PreLoadCheck()
{
    if (!TestAllocator())
    {
        error("[ModManager] Issue with allocator instance (how did we not crash yet?)");
        return false;
    }
    return true;
}

void OnModsLoaded() {}

void OnModsUnloading()
{
    JsonHook::Teardown();  // restore ExecFunction/flags before DLL pages are freed
}

void OnWorldChanged()
{
    // World transitioned — cached call targets (raw UObject*/UFunction*) are now
    // stale; drop them so `call` cleanly rescans instead of touching freed memory.
    State::ScannedFunctions.clear();
    State::ScannedFunctionVariantsByName.clear();
}

#pragma pop_macro("EOF")