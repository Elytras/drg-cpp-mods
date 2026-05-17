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
concept IsUObject = std::is_base_of_v<UObject, T>;

template<typename T>
concept DerivedUObject =
    IsUObject<T> &&
    !std::is_same_v<T, UObject> &&
    requires { { T::StaticClass() } -> std::same_as<UClass*>; };

template<typename T>
concept IsAActor = std::is_base_of_v<AActor, T>;

template<typename T>
concept IsAItem = std::is_base_of_v<AItem, T>;

template<typename T>
concept IsUComponent = std::is_base_of_v<UActorComponent, T>;

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
    const UClass* derived;
    const UClass* base;
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
    bool IsSubclassOf(const UClass* derived, const UClass* base);
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
FString      ToFString(std::string_view str);

// =========================================================================
// Core helpers
// =========================================================================

std::string GetDisplayName (UObject* Obj);
bool        IsInActiveWorld(UObject* obj);
void        SleepNow       (uint64_t ms);
uint64_t    GetTimeMs      ();
void        Exec           (std::string cmd);
bool        NearlyEqual    (double a, double b, double epsilon = 1e-9);
bool        IsOnSpacerig   ();
inline bool IsOnSpaceRig() { return IsOnSpacerig(); }

// =========================================================================
// Templates
// =========================================================================

template<typename T>
    requires DerivedUObject<T>
bool IsChildOf(UClass* Test)
{
    UClass* Base = T::StaticClass();
    if (!Base || !Test) return false;
    return MathLib::ClassIsChildOf(Test, Base);
}

template<typename T>
bool IsValidOf(const UObject* Target)
{
    if (!Target) return false;
    if (UObject::GObjects.GetTypedPtr()->GetByIndex(Target->Index) != Target) return false;
    if (!IsValidRaw(Target) || !IsValid(Target)) return false;
    if constexpr (DerivedUObject<T>) { if (!IsChildOf<T>(Target->Class)) return false; }
    return true;
}

template<typename UEType>
    requires DerivedUObject<UEType>
UEType* GetTypedOuter(UObject* Obj)
{
    if (!Obj || !IsValid(Obj)) return nullptr;
    const UClass* Class = UEType::StaticClass();
    for (UObject* Outer = Obj->Outer; Outer; Outer = Outer->Outer)
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

template<typename AActorType = AActor>
    requires IsAActor<AActorType>
AActorType* GetActorOfClass(UClass* Class = AActorType::StaticClass())
{
    if (!IsValidClass(Class) || !(Class->IsA(EClassCastFlags::Actor) || Class->IsSubclassOf(AActor::StaticClass())))
    {
        spdlog::warn("GetActorOfClass: Invalid class");
        return nullptr;
    }
    for (AActor* Object : GObjectsOf<AActor>())
    {
        if ((Object->Class->IsSubclassOf(Class) || Object->Class->IsA(Class)) && !Object->IsDefaultObject())
            return static_cast<AActorType*>(Object);
    }
    spdlog::warn("GetActorOfClass: No actor of class {} found", Class->StaticName().ToString());
    return nullptr;
}

template<typename TComponentClass = UActorComponent>
    requires IsUComponent<TComponentClass>
TComponentClass* AttachComponent(AActor* Actor, TSubclassOf<TComponentClass> ComponentClass)
{
    if (!IsValid(Actor) || !IsValidClass(ComponentClass)) return nullptr;
    if (!ComponentClass->IsSubclassOf(UActorComponent::StaticClass())) return nullptr;
    return Actor->AddComponentByClass(ComponentClass, false, FTransform(), false);
}

template<typename T>
    requires IsAItem<T>
T* GetItem(APlayerCharacter* Target, EItemCategory Category)
{
    if (Category == EItemCategory::EItemCategory_MAX) return nullptr;
    if (!IsValidOf<APlayerCharacter>(Target)) return nullptr;
    if (!Target->InventoryComponent) return nullptr;
    AItem* Item = Target->InventoryComponent->GetItem(Category);
    if (!IsValidOf<AItem>(Item)) return nullptr;
    return ObjectCast::Cast<T>(Item);
}

template<typename T> T* GetPrimaryWeapon  (APlayerCharacter* P) { return GetItem<T>(P, EItemCategory::PrimaryWeapon); }
template<typename T> T* GetSecondaryWeapon(APlayerCharacter* P) { return GetItem<T>(P, EItemCategory::SecondaryWeapon); }
template<typename T> T* GetTraversalTool  (APlayerCharacter* P) { return GetItem<T>(P, EItemCategory::TraversalTool); }
template<typename T> T* GetClassTool      (APlayerCharacter* P) { return GetItem<T>(P, EItemCategory::ClassTool); }
template<typename T> T* GetGrenade        (APlayerCharacter* P) { return GetItem<T>(P, EItemCategory::Grenade); }
template<typename T> T* GetFlare          (APlayerCharacter* P) { return GetItem<T>(P, EItemCategory::Flare); }
template<typename T> T* GetMiningTool     (APlayerCharacter* P) { return GetItem<T>(P, EItemCategory::MiningTool); }
template<typename T> T* GetArmor          (APlayerCharacter* P) { return GetItem<T>(P, EItemCategory::Armor); }

template<typename T = AActor>
    requires IsAActor<T>
bool SpawnActor(TSubclassOf<T> ActorClass, const FTransform& SpawnTransform, T*& OutActor)
{
    OutActor = nullptr;
    if (!ActorClass || !IsValidClass(ActorClass) || !MathLib::ClassIsChildOf(ActorClass, AActor::StaticClass()))
        return false;
    AActor* TempActor = UGameplayStatics::BeginDeferredActorSpawnFromClass(
        GetWorld(), ActorClass, SpawnTransform,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);
    if (!TempActor) return false;
    OutActor = ObjectCast::Cast<T>(UGameplayStatics::FinishSpawningActor(TempActor, SpawnTransform));
    return OutActor != nullptr;
}

template<typename T >
    requires IsUComponent<T>
T* GetComponent(AActor* Actor) {
    return ObjectCast::Cast<T>(Actor->GetComponentByClass(T::StaticClass()));
}

template<typename T = AActor>
    requires IsAActor<T>
TArray<AActor*> GetAllActorsOfClass()
{
    TArray<AActor*> Out;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), T::StaticClass(), &Out);
    return Out;
}

// =========================================================================
// Player helpers
// =========================================================================

AFSDPlayerController* GetLocalController();
APlayerCharacter* GetLocalPlayerCharacterBlocking(uint64_t MaxWaitMs);
APlayerCharacter* GetLocalPlayer();
AFSDPlayerState*  GetLocalPlayerState();

// =========================================================================
// Misc non-template helpers
// =========================================================================

std::vector<AFSDPawn*> GetAliveNonFriendlies();
std::string            ObjToStr    (const UObject* Obj);
std::string            parse_quoted(std::string_view input);
void*                  FindPattern (const wchar_t* moduleName, std::string_view pattern);

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
