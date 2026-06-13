#pragma once
// Lib_Utils.h — SubclassCache, safe parsers, general helpers, player access.
#include <string>
#include <cstdint>
#include <concepts>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <type_traits>
#include "Lib_Forward.h"
#include "Lib_ObjectCast.h"
#include "Lib_VTableInfo.h"

// =========================================================================
// Concepts
// =========================================================================

template<typename T>
concept IsUObject = std::is_base_of_v<SDK::UObject, T>;

template<typename T>
concept DerivedUObject =
    IsUObject<T> &&
    !std::is_same_v<T, SDK::UObject> &&
    requires { { T::StaticClass() } -> std::same_as<SDK::UClass*>; };

template<typename T>
concept IsAActor = std::is_base_of_v<SDK::AActor, T>;

template<typename T>
concept IsAItem = std::is_base_of_v<SDK::AItem, T>;

template<typename T>
concept IsUComponent = std::is_base_of_v<SDK::UActorComponent, T>;

// =========================================================================
// SubclassCache
// =========================================================================

struct PairKey
{
    const SDK::UClass* derived;
    const SDK::UClass* base;
    bool operator==(const PairKey& o) const { return derived == o.derived && base == o.base; }
};

struct PairKeyHash
{
    size_t operator()(const PairKey& k) const noexcept
    {
        uint64_t a = reinterpret_cast<uint64_t>(k.derived);
        uint64_t b = reinterpret_cast<uint64_t>(k.base);
        return std::hash<uint64_t>{}(a * 2654435761ULL ^ b * 2246822519ULL);
    }
};

class SubclassCache
{
public:
    static SubclassCache& Get() { static SubclassCache i; return i; }
    bool IsSubclassOf(const SDK::UClass* derived, const SDK::UClass* base);
    void Clear();
private:
    SubclassCache() { cache_.reserve(256); }
    std::shared_mutex                              mutex_;
    std::unordered_map<PairKey, bool, PairKeyHash> cache_;
};

// Game-agnostic helpers (SafeSto*, NoneOf/AnyOf, SleepNow/GetTimeMs, NearlyEqual,
// IsIndexableContainer/GetOrDefault, numeric_range/range) live in the shared core
// layer so SharedLib code can use them too.
#include "CoreUtils.h"

// =========================================================================
// String / FString
// =========================================================================

std::wstring ToWide   (std::string_view in);
SDK::FString ToFString(std::string_view str);

// =========================================================================
// Core helpers
// =========================================================================

std::string GetDisplayName (SDK::UObject* Obj);
bool        IsInActiveWorld(SDK::UObject* obj);
void        Exec           (std::string cmd);
bool        IsOnSpacerig   ();
inline bool IsOnSpaceRig() { return IsOnSpacerig(); }

// =========================================================================
// Templates
// =========================================================================

template<typename T>
    requires DerivedUObject<T>
bool IsChildOf(SDK::UClass* Test)
{
    SDK::UClass* Base = T::StaticClass();
    if (!Base || !Test) return false;
    return MathLib::ClassIsChildOf(Test, Base);
}

template<typename T>
bool IsValidOf(const SDK::UObject* Target)
{
    if (!Target) return false;
    if (SDK::UObject::GObjects.GetTypedPtr()->GetByIndex(Target->Index) != Target) return false;
    if (!IsValidRaw(Target) || !IsValid(Target)) return false;
    if constexpr (DerivedUObject<T>) { if (!IsChildOf<T>(Target->Class)) return false; }
    return true;
}

template<typename UEType>
    requires DerivedUObject<UEType>
UEType* GetTypedOuter(SDK::UObject* Obj)
{
    if (!Obj || !IsValid(Obj)) return nullptr;
    const SDK::UClass* Class = UEType::StaticClass();
    for (SDK::UObject* Outer = Obj->Outer; Outer; Outer = Outer->Outer)
        if (Outer->IsA(Class)) return static_cast<UEType*>(Outer);
    return nullptr;
}

template<typename AActorType = SDK::AActor>
    requires IsAActor<AActorType>
AActorType* GetActorOfClass(SDK::UClass* Class = AActorType::StaticClass())
{
    if (!IsValidClass(Class) || !(Class->IsA(SDK::EClassCastFlags::Actor) || Class->IsSubclassOf(SDK::AActor::StaticClass())))
    {
        warn("GetActorOfClass: Invalid class");
        return nullptr;
    }
    for (SDK::AActor* Object : GObjectsOf<SDK::AActor>())
    {
        // IsDefaultObject() is the cheap check, so it gates the subclass walk.
        // (The old `|| Object->Class->IsA(Class)` arm was redundant: it asked
        // whether the UClass object itself is an instance of Class — near-always
        // false — so IsSubclassOf alone is the correct and complete test.)
        if (!Object->IsDefaultObject() && Object->Class->IsSubclassOf(Class))
            return static_cast<AActorType*>(Object);
    }
    warn("GetActorOfClass: No actor of class {} found", Class->StaticName().ToString());
    return nullptr;
}

template<typename TComponentClass = SDK::UActorComponent>
    requires IsUComponent<TComponentClass>
TComponentClass* AttachComponent(SDK::AActor* Actor, SDK::TSubclassOf<TComponentClass> ComponentClass)
{
    if (!IsValid(Actor) || !IsValidClass(ComponentClass)) return nullptr;
    if (!ComponentClass->IsSubclassOf(SDK::UActorComponent::StaticClass())) return nullptr;
    return Actor->AddComponentByClass(ComponentClass, false, SDK::FTransform(), false);
}

template<typename T>
    requires IsAItem<T>
T* GetItem(SDK::APlayerCharacter* Target, SDK::EItemCategory Category)
{
    if (Category == SDK::EItemCategory::EItemCategory_MAX) return nullptr;
    if (!IsValidOf<SDK::APlayerCharacter>(Target)) return nullptr;
    if (!Target->InventoryComponent) return nullptr;
    SDK::AItem* Item = Target->InventoryComponent->GetItem(Category);
    if (!IsValidOf<SDK::AItem>(Item)) return nullptr;
    return ObjectCast::Cast<T>(Item);
}

template<typename T> T* GetPrimaryWeapon  (SDK::APlayerCharacter* P) { return GetItem<T>(P, SDK::EItemCategory::PrimaryWeapon); }
template<typename T> T* GetSecondaryWeapon(SDK::APlayerCharacter* P) { return GetItem<T>(P, SDK::EItemCategory::SecondaryWeapon); }
template<typename T> T* GetTraversalTool  (SDK::APlayerCharacter* P) { return GetItem<T>(P, SDK::EItemCategory::TraversalTool); }
template<typename T> T* GetClassTool      (SDK::APlayerCharacter* P) { return GetItem<T>(P, SDK::EItemCategory::ClassTool); }
template<typename T> T* GetGrenade        (SDK::APlayerCharacter* P) { return GetItem<T>(P, SDK::EItemCategory::Grenade); }
template<typename T> T* GetFlare          (SDK::APlayerCharacter* P) { return GetItem<T>(P, SDK::EItemCategory::Flare); }
template<typename T> T* GetMiningTool     (SDK::APlayerCharacter* P) { return GetItem<T>(P, SDK::EItemCategory::MiningTool); }
template<typename T> T* GetArmor          (SDK::APlayerCharacter* P) { return GetItem<T>(P, SDK::EItemCategory::Armor); }

template<typename T = SDK::AActor>
    requires IsAActor<T>
bool SpawnActor(SDK::TSubclassOf<T> ActorClass, const SDK::FTransform& SpawnTransform, T*& OutActor)
{
    OutActor = nullptr;
    if (!ActorClass || !IsValidClass(ActorClass) || !MathLib::ClassIsChildOf(ActorClass, SDK::AActor::StaticClass()))
        return false;
    SDK::AActor* TempActor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        GetWorld(), ActorClass, SpawnTransform,
        SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);
    if (!TempActor) return false;
    OutActor = ObjectCast::Cast<T>(SDK::UGameplayStatics::FinishSpawningActor(TempActor, SpawnTransform));
    return OutActor != nullptr;
}

template<typename T >
    requires IsUComponent<T>
T* GetComponent(SDK::AActor* Actor) {
    if (!Actor) return nullptr;
    return ObjectCast::Cast<T>(Actor->GetComponentByClass(T::StaticClass()));
}

template<typename T = SDK::AActor>
    requires IsAActor<T>
SDK::TArray<SDK::AActor*> GetAllActorsOfClass()
{
    SDK::TArray<SDK::AActor*> Out;
    SDK::UGameplayStatics::GetAllActorsOfClass(GetWorld(), T::StaticClass(), &Out);
    return Out;
}

// =========================================================================
// Player helpers
// =========================================================================

SDK::AFSDPlayerController* GetLocalController();
SDK::APlayerCharacter*     GetLocalPlayerCharacterBlocking(uint64_t MaxWaitMs);
SDK::APlayerCharacter*     GetLocalPlayer();
SDK::AFSDPlayerState*      GetLocalPlayerState();

// =========================================================================
// Misc non-template helpers
// =========================================================================

std::vector<SDK::AFSDPawn*> GetAliveNonFriendlies();
std::string                ObjToStr    (const SDK::UObject* Obj);
std::string            parse_quoted(std::string_view input);
void*                  FindPattern (const wchar_t* moduleName, std::string_view pattern);
void*                  FindPatternNoAlloc(const wchar_t* moduleName, std::string_view pattern);
