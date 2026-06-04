#pragma once
// Lib_Utils.h — SubclassCache, safe parsers, general helpers, player access (RogueCore).
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
#include "../SharedLib/StringLib.h"
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
concept IsUComponent = std::is_base_of_v<SDK::UActorComponent, T>;

template<typename T>
concept IsIndexableContainer = requires (const T& c, size_t i) {
    typename T::value_type;
    { c.size()  } -> std::same_as<size_t>;
    { c.empty() } -> std::same_as<bool>;
    { c[i]      } -> std::convertible_to<typename T::value_type>;
};

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

// =========================================================================
// Safe parsers
// =========================================================================

int64_t  SafeStoll (const std::string& s) noexcept;
uint64_t SafeStoull(const std::string& s) noexcept;
float    SafeStof  (const std::string& s) noexcept;
double   SafeStod  (const std::string& s) noexcept;

// =========================================================================
// Variadic helpers
// =========================================================================

template<typename T, typename... Ts> bool NoneOf(T c, Ts... ts) { return ((c != ts) && ...); }
template<typename T, typename... Ts> bool AnyOf (T c, Ts... ts) { return ((c == ts) || ...); }

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
void        SleepNow       (uint64_t ms);
uint64_t    GetTimeMs      ();
void        Exec           (std::string cmd);
bool        NearlyEqual    (double a, double b, double epsilon = 1e-9);

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

template<IsIndexableContainer Container>
typename Container::value_type GetOrDefault(
    const Container& c,
    const typename Container::value_type& def,
    std::optional<size_t> index = std::nullopt)
{
    if (!c.empty())
    {
        if (index && *index < c.size()) return c[*index];
        return c[0];
    }
    return def;
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

// =========================================================================
// Player helpers
// =========================================================================

SDK::APlayerController* GetLocalController();
SDK::APlayerCharacter*  GetLocalPlayer();

// Poll GetLocalPlayer() until it returns non-null or `MaxWaitMs` elapses.
// Returns the resolved player or nullptr on timeout.
SDK::APlayerCharacter*  GetLocalPlayerCharacterBlocking(uint64_t MaxWaitMs);

// =========================================================================
// DRG-only helpers — declared here as `static_assert` stubs so RC code that
// accidentally calls one gets a focused compile error explaining why, instead
// of an "undeclared identifier" or cryptic SFINAE failure. Implement on the
// RC side (with whatever the RC equivalent is) before using.
// =========================================================================

namespace _rc_stubs {
    template<typename T = void> struct dependent_false : std::false_type {};
}

#define RC_DRG_ONLY(MSG)                                                       \
    static_assert(::_rc_stubs::dependent_false<T>::value,                       \
        "RogueCore: " MSG " — DRG-specific helper, not implemented for RC.")

// `prop`-style item access (DRG inventory model: APlayerCharacter +
// InventoryComponent + EItemCategory). RC has a different item system.
template<typename T> T* GetItem(SDK::APlayerCharacter*, int /*category*/)
{ RC_DRG_ONLY("GetItem<T>"); return nullptr; }

template<typename T> T* GetPrimaryWeapon  (SDK::APlayerCharacter*) { RC_DRG_ONLY("GetPrimaryWeapon");   return nullptr; }
template<typename T> T* GetSecondaryWeapon(SDK::APlayerCharacter*) { RC_DRG_ONLY("GetSecondaryWeapon"); return nullptr; }
template<typename T> T* GetTraversalTool  (SDK::APlayerCharacter*) { RC_DRG_ONLY("GetTraversalTool");   return nullptr; }
template<typename T> T* GetClassTool      (SDK::APlayerCharacter*) { RC_DRG_ONLY("GetClassTool");       return nullptr; }
template<typename T> T* GetGrenade        (SDK::APlayerCharacter*) { RC_DRG_ONLY("GetGrenade");         return nullptr; }
template<typename T> T* GetFlare          (SDK::APlayerCharacter*) { RC_DRG_ONLY("GetFlare");           return nullptr; }
template<typename T> T* GetMiningTool     (SDK::APlayerCharacter*) { RC_DRG_ONLY("GetMiningTool");      return nullptr; }
template<typename T> T* GetArmor          (SDK::APlayerCharacter*) { RC_DRG_ONLY("GetArmor");           return nullptr; }

// Map-state queries — "space rig" is a DRG-only concept (the hub map between
// missions). RC has no analogous notion.
template<typename T = void> bool IsOnSpacerig() { RC_DRG_ONLY("IsOnSpacerig"); return false; }
template<typename T = void> bool IsOnSpaceRig() { RC_DRG_ONLY("IsOnSpaceRig"); return false; }

// Player-state subclass — DRG's AFSDPlayerState carries DRG-specific data
// (perks, vanity, etc.). RC code wanting the player state should use the
// engine's APlayerState directly via GetLocalController()->PlayerState.
template<typename T = void> SDK::APlayerState* GetLocalPlayerState()
{ RC_DRG_ONLY("GetLocalPlayerState"); return nullptr; }

// Enemy enumeration — depends on AFSDPawn / DRG enemy faction system.
template<typename T = void> std::vector<SDK::AActor*> GetAliveNonFriendlies()
{ RC_DRG_ONLY("GetAliveNonFriendlies"); return {}; }

#undef RC_DRG_ONLY

// =========================================================================
// Misc non-template helpers
// =========================================================================

std::string ObjToStr    (const SDK::UObject* Obj);
std::string parse_quoted(std::string_view input);
void*       FindPattern (const wchar_t* moduleName, std::string_view pattern);

// =========================================================================
// Numeric ranges
//
//   numeric_range<T>(begin, end)         — step ±1 by direction
//   numeric_range<T>(begin, end, step)   — explicit step
//   range(begin, end)  /  range(begin, end, step)  /  range(end)
// =========================================================================

namespace nr_detail {

    template<typename T>
    concept NumericPrimitive =
        (std::integral<T>
            && !std::same_as<T, bool>
            && !std::same_as<T, wchar_t>
            && !std::same_as<T, char8_t>
            && !std::same_as<T, char16_t>
            && !std::same_as<T, char32_t>)
        || std::floating_point<T>
#if defined(__FLT16_MAX__)
        || std::same_as<T, _Float16>
#endif
        ;

    template<NumericPrimitive T>
    constexpr T default_step() noexcept { return static_cast<T>(1); }

} // namespace nr_detail


template<nr_detail::NumericPrimitive T>
class numeric_range_iterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type        = T;
    using difference_type   = std::ptrdiff_t;
    using pointer           = const T*;
    using reference         = T;

    constexpr numeric_range_iterator(T cur, T end, T step) noexcept
        : cur_(cur), end_(end), step_(step) {}

    constexpr T       operator*()  const noexcept { return cur_; }
    constexpr pointer operator->() const noexcept { return &cur_; }

    constexpr numeric_range_iterator& operator++() noexcept { cur_ = static_cast<T>(cur_ + step_); return *this; }
    constexpr numeric_range_iterator  operator++(int) noexcept { auto c = *this; ++(*this); return c; }

    constexpr bool operator==(const numeric_range_iterator& o) const noexcept {
        return step_ > T{0} ? cur_ >= o.cur_ : cur_ <= o.cur_;
    }
    constexpr bool operator!=(const numeric_range_iterator& o) const noexcept { return !(*this == o); }

private:
    T cur_, end_, step_;
};


template<nr_detail::NumericPrimitive T>
class numeric_range {
public:
    using iterator = numeric_range_iterator<T>;

    constexpr numeric_range(T begin, T end)
        : begin_(begin), end_(end)
        , step_(end >= begin ? nr_detail::default_step<T>() : static_cast<T>(-1)) {}

    constexpr numeric_range(T begin, T end, T step)
        : begin_(begin), end_(end), step_(step)
    {
        if (step == T{0}) throw std::invalid_argument("numeric_range: step must not be zero");
    }

    constexpr iterator begin() const noexcept {
        if (step_ > T{0} && begin_ >= end_) return sentinel();
        if (step_ < T{0} && begin_ <= end_) return sentinel();
        return iterator{ begin_, end_, step_ };
    }
    constexpr iterator end()   const noexcept { return sentinel(); }

    constexpr bool empty() const noexcept {
        if (step_ > T{0}) return begin_ >= end_;
        if (step_ < T{0}) return begin_ <= end_;
        return true;
    }

    constexpr std::ptrdiff_t size() const noexcept requires std::integral<T> {
        if (step_ > T{0} && end_ > begin_)
            return static_cast<std::ptrdiff_t>((end_ - begin_ + step_ - T{1}) / step_);
        if (step_ < T{0} && begin_ > end_)
            return static_cast<std::ptrdiff_t>((begin_ - end_ - step_ - T{1}) / (-step_));
        return 0;
    }

private:
    constexpr iterator sentinel() const noexcept { return iterator{ end_, end_, step_ }; }
    T begin_, end_, step_;
};

template<nr_detail::NumericPrimitive T> numeric_range(T, T)    -> numeric_range<T>;
template<nr_detail::NumericPrimitive T> numeric_range(T, T, T) -> numeric_range<T>;

template<nr_detail::NumericPrimitive T> constexpr auto range(T begin, T end)        { return numeric_range<T>(begin, end); }
template<nr_detail::NumericPrimitive T> constexpr auto range(T begin, T end, T step){ return numeric_range<T>(begin, end, step); }
template<nr_detail::NumericPrimitive T> constexpr auto range(T end)                 { return numeric_range<T>(T{0}, end); }
