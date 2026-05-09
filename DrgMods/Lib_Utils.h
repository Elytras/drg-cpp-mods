#pragma once
// Lib_Utils.h — SubclassCache, safe parsers, general helpers, player access.
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <cstdint>
#include <concepts>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <type_traits>
#include "Lib_Forward.h"
#include "Lib_FField.h"
#include "Lib_ObjectCast.h"
#include "Lib_VTableInfo.h"
#include "SDK/UtfN.hpp"
#include "SDK/SDK/AssetRegistry_classes.hpp"
#include "SDK/SDK/AssetRegistry_structs.hpp"

// =========================================================================
// Concepts
// =========================================================================
template<typename T>
concept IsUObject = std::is_base_of_v<UObject, T>;

template<typename T>
concept DerivedUObject = 
IsUObject<T> && 
!std::is_same_v<T, UObject> && 
requires {
    { 
        T::StaticClass() 
    } -> std::same_as<UClass*>;
};

template<typename T>
concept IsAActor = std::is_base_of_v<AActor, T>;

template<typename T>
concept IsAItem = std::is_base_of_v<AItem, T>;

template<typename T>
concept IsUComponent = std::is_base_of_v<UActorComponent, T>;

template<typename T>
concept IsIndexableContainer = requires (const T & c, size_t i) {
    typename T::value_type;
    { c.size() } -> std::same_as<size_t>;
    { c.empty() } -> std::same_as<bool>;
    { c[i] } -> std::convertible_to<typename T::value_type>;
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

    bool IsSubclassOf(const UClass* derived, const UClass* base)
    {
        if (!derived || !base) return false;
        if (derived == base)   return true;
        PairKey key{ derived, base };
        { std::shared_lock lock(mutex_); if (auto it = cache_.find(key); it != cache_.end()) return it->second; }
        bool result = derived->IsSubclassOf(base);
        { std::unique_lock lock(mutex_); cache_.emplace(key, result); }
        return result;
    }

    void Clear() { std::unique_lock lock(mutex_); cache_.clear(); }

private:
    SubclassCache() { cache_.reserve(256); }
    std::shared_mutex                              mutex_;
    std::unordered_map<PairKey, bool, PairKeyHash> cache_;
};

// =========================================================================
// Safe parsers
// =========================================================================

inline int64_t  SafeStoll(const std::string& s) { if (s.empty()) return 0;   try { return std::stoll(s); } catch (...) { return 0; } }
inline uint64_t SafeStoull(const std::string& s) { if (s.empty()) return 0;   try { return std::stoull(s); } catch (...) { return 0; } }
inline float    SafeStof(const std::string& s) { if (s.empty()) return 0.f; try { return std::stof(s); } catch (...) { return 0.f; } }
inline double   SafeStod(const std::string& s) { if (s.empty()) return 0.0; try { return std::stod(s); } catch (...) { return 0.0; } }

// =========================================================================
// Variadic helpers
// =========================================================================

template<typename T, typename... Ts> bool NoneOf(T c, Ts... ts) { return ((c != ts) && ...); }
template<typename T, typename... Ts> bool AnyOf(T c, Ts... ts) { return ((c == ts) || ...); }

// =========================================================================
// String / FString
// =========================================================================

inline std::wstring ToWide(std::string_view in) { return StringLib::ToWide(in); }
inline FString      ToFString(std::string_view str) { return FString(ToWide(str).c_str()); }

// =========================================================================
// Core helpers (definitions for forward decls in Lib_Forward.h)
// =========================================================================

inline UWorld* GetWorld() { return UWorld::GetWorld(); }
inline bool    IsValid(const UObject* Obj) { return Kismet::IsValid(Obj); }
inline bool    IsValidClass(const UClass* Class) { return Kismet::IsValidClass(const_cast<UClass*>(Class)); }
inline std::string GetDisplayName(UObject* Obj) { return Kismet::GetDisplayName(Obj).ToString(); }

inline bool IsInActiveWorld(UObject* obj)
{
    UWorld* world = UWorld::GetWorld();
    if (!world) return true;
    for (UObject* outer = obj->Outer; outer; outer = outer->Outer)
        if (outer == world) return true;
    return false;
}

inline void     SleepNow(uint64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds{ ms }); }
inline uint64_t GetTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void Exec(std::string cmd)
{
    Kismet::ExecuteConsoleCommand(GetWorld(), FString(StringLib::ToWide(cmd).c_str()), nullptr);
}

// =========================================================================
// Templates
// =========================================================================

template<typename T>
    requires DerivedUObject<T>
inline bool IsChildOf(UClass* Test)
{
    UClass* Base = T::StaticClass();
    if (!Base) return false;
    if (!Test) return false;

    if (!MathLib::ClassIsChildOf(Test, Base))   return false;
    return true;
}

template<typename T>
inline bool IsValidOf(const UObject* Target)
{
    if (!Target) return false;
    if (UObject::GObjects.GetTypedPtr()->GetByIndex(Target->Index) != Target) return false;
    if (!IsValidRaw(Target) || !IsValid(Target)) return false;
    if constexpr (DerivedUObject<T>) { if (!IsChildOf<T>(Target->Class))  return false; }
    return true;
}

template<typename UEType>
    requires DerivedUObject<UEType>
inline UEType* GetTypedOuter(UObject* Obj)
{
    if (!Obj || !IsValid(Obj)) return nullptr;
    const UClass* Class = UEType::StaticClass();
    for (UObject* Outer = Obj->Outer; Outer; Outer = Outer->Outer)
        if (Outer->IsA(Class)) return static_cast<UEType*>(Outer);
    return nullptr;
}

template<IsIndexableContainer Container>
inline typename Container::value_type GetOrDefault(
    const Container& c,
    const typename Container::value_type& def,
    std::optional<size_t> index = std::nullopt)
{
    if (!c.empty())
    {
        if (index && *index < c.size())
            return c[*index];
        return c[0];
    }
    return def;
}

template<typename AActorType = AActor>
    requires IsAActor<AActorType>
inline AActorType* GetActorOfClass(UClass* Class = AActorType::StaticClass())
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

template <typename TComponentClass = UActorComponent>
    requires IsUComponent<TComponentClass>
inline TComponentClass* AttachComponent(AActor* Actor, TSubclassOf<TComponentClass> ComponentClass)
{
    if (!IsValid(Actor) || !IsValidClass(ComponentClass)) return nullptr;
    if (!ComponentClass->IsSubclassOf(UActorComponent::StaticClass())) return nullptr;
    return Actor->AddComponentByClass(ComponentClass, false, FTransform(), false);
}

template<typename T>
    requires IsAItem<T>
inline T* GetItem(APlayerCharacter* Target, EItemCategory Category)
{
    if (Category == EItemCategory::EItemCategory_MAX) return nullptr;
    if (!IsValidOf<APlayerCharacter>(Target)) return nullptr;
    if (!Target->InventoryComponent) return nullptr;

    AItem* Item = Target->InventoryComponent->GetItem(Category);
    if (!IsValidOf<AItem>(Item)) return nullptr;

    return ObjectCast::Cast<T>(Item);
}

template<typename T>
inline T* GetPrimaryWeapon(APlayerCharacter* Player) {
    return GetItem<T>(Player, EItemCategory::PrimaryWeapon);
}

template<typename T>
inline T* GetSecondaryWeapon(APlayerCharacter* Player) {
    return GetItem<T>(Player, EItemCategory::SecondaryWeapon);
}

template<typename T>
inline T* GetTraversalTool(APlayerCharacter* Player) {
    return GetItem<T>(Player, EItemCategory::TraversalTool);
}

template<typename T>
inline T* GetClassTool(APlayerCharacter* Player) {
    return GetItem<T>(Player, EItemCategory::ClassTool);
}

template<typename T>
inline T* GetGrenade(APlayerCharacter* Player) {
    return GetItem<T>(Player, EItemCategory::Grenade);
}

template<typename T>
inline T* GetFlare(APlayerCharacter* Player) {
    return GetItem<T>(Player, EItemCategory::Flare);
}

template<typename T>
inline T* GetMiningTool(APlayerCharacter* Player) {
    return GetItem<T>(Player, EItemCategory::MiningTool);
}

template<typename T>
inline T* GetArmor(APlayerCharacter* Player) {
    return GetItem<T>(Player, EItemCategory::Armor);
}

template<typename T = AActor>
    requires IsAActor<T>
inline bool SpawnActor(TSubclassOf<T> ActorClass, const FTransform& SpawnTransform, T*& OutActor)
{
    OutActor = nullptr;

    if (!ActorClass || !IsValidClass(ActorClass) || !MathLib::ClassIsChildOf(ActorClass, AActor::StaticClass()))
        return false;

    AActor* TempActor = UGameplayStatics::BeginDeferredActorSpawnFromClass(
        GetWorld(),
        ActorClass,
        SpawnTransform,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
        nullptr
    );

    if (!TempActor) return false;

    OutActor = ObjectCast::Cast<T>(UGameplayStatics::FinishSpawningActor(TempActor, SpawnTransform));
    return OutActor != nullptr;
}

// =========================================================================
// Player helpers
// =========================================================================

inline APlayerCharacter* GetLocalPlayerCharacterBlocking(uint64_t MaxWaitMs)
{
    UWorld* World = GetWorld();
    if (!IsValid(World)) return spdlog::warn("GetLocalPlayerCharacterBlocking: Invalid World"), nullptr;
    const uint64_t start = GetTimeMs();
    APlayerCharacter* Player = nullptr;
    while ((GetTimeMs() - start) < MaxWaitMs)
    {
        Player = GameLib::GetLocalPlayerCharacter(World);
        if (Player && IsValid(Player) && MathLib::ClassIsChildOf(Player->Class, Player->StaticClass()))
            break;
        Player = nullptr;
        SleepNow(16);
    }
    return Player;
}

inline APlayerCharacter* GetLocalPlayer()
{
    return GameLib::GetLocalPlayerCharacter(GetWorld());
}

inline AFSDPlayerState* GetLocalPlayerState() 
{   
    AFSDPlayerState* OutState = nullptr;
    UWorld* World = GetWorld();


    return OutState;
}

// =========================================================================
// Asset registry
// =========================================================================

//template<typename Class = UClass>
//inline TArray<Class*> GetAssetsOfClassByName(const TArray<FName>& Names, bool ForceRescan)
//{
//    TArray<Class*> Result{};
//    auto* Engine = UEngine::GetEngine();
//    if (!Engine) { spdlog::error("GetAssetsOfClassByName: Engine not initialized"); return Result; }
//    auto* AR = UAssetRegistryHelpers::GetAssetRegistry();
//    if (ForceRescan) AR->ScanPathsSynchronous({ "/Game" }, true);
//    TArray<FAssetData> AssetArray{};
//    FARFilter Filter{};
//    Filter.ClassNames = Names; Filter.bRecursivePaths = true; Filter.bRecursiveClasses = true;
//    AR->GetAssets(Filter, &AssetArray);
//    std::string nameList;
//    for (int32 i = 0; i < Names.Num(); ++i) { nameList += Names[i].ToString(); if (i + 1 < Names.Num()) nameList += ", "; }
//    spdlog::info("GetAssetsOfClassByName: Found {} assets for [{}]", AssetArray.Num(), nameList);
//    return Result;
//}

//template<typename T>
//inline int32 ArrayAdd(TArray<T>& arr, const T& item)
//{
//    return UKismetArrayLibrary::Array_Add(
//        reinterpret_cast<const TArray<int32>&>(arr),
//        reinterpret_cast<const int32&>(item)
//    );
//}

namespace {
    inline UClass* FindClass(std::string_view name)
    {
        const FName needle(ToWide(name).c_str());
        for (UClass* cls : GObjectsOf<UClass>())
            if (cls->Name == needle) return cls;
        return nullptr;
    }
}


// =========================================================================
// Pattern scanner
// =========================================================================

// Scans all executable sections of a loaded module for a byte pattern.
// Pattern format: space-separated hex bytes, "??" = wildcard.
// Returns the address of the first match, or nullptr.
inline void* FindPattern(const wchar_t* moduleName, std::string_view pattern)
{
    HMODULE mod = GetModuleHandleW(moduleName);
    if (!mod) return nullptr;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);

    std::vector<uint8_t> pat;
    std::vector<bool>    mask;
    const char* s = pattern.data();
    const char* e = s + pattern.size();
    while (s < e)
    {
        while (s < e && *s == ' ') ++s;
        if (s >= e) break;
        if (s[0] == '?' && s + 1 < e && s[1] == '?')
        {
            pat.push_back(0x00); mask.push_back(false); s += 2;
        }
        else
        {
            char hex[3] = { s[0], s[1], '\0' };
            pat.push_back(static_cast<uint8_t>(std::strtoul(hex, nullptr, 16)));
            mask.push_back(true);
            s += 2;
        }
    }

    const size_t len = pat.size();
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (!(sec->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)))
            continue;

        const uint8_t* begin = reinterpret_cast<uint8_t*>(mod) + sec->VirtualAddress;
        const uint8_t* last  = begin + sec->Misc.VirtualSize - len;
        for (const uint8_t* p = begin; p <= last; ++p)
        {
            bool match = true;
            for (size_t j = 0; j < len && match; ++j)
                if (mask[j] && p[j] != pat[j]) match = false;
            if (match) return const_cast<uint8_t*>(p);
        }
    }
    return nullptr;
}

inline bool NearlyEqual(double a, double b, double epsilon = 1e-9) {
    return std::fabs(a - b) <= epsilon * (std::max)({ 1.0, std::fabs(a), std::fabs(b) });
}

inline std::vector<AFSDPawn*> GetAliveNonFriendlies() {
    std::vector<AFSDPawn*> Actors{};
    TArray<AActor*> Out;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AFSDPawn::StaticClass(), &Out);
    Actors.reserve(Out.Num());
    for (auto A : Out) {
        auto Enemy = ObjectCast::Cast<AFSDPawn>(A);
        if (!IsValidOf<AFSDPawn>(Enemy)) continue;
        if (!Enemy->GetHealthComponent()->IsAlive()) continue;
        if ((Enemy->GetAttitude() == EPawnAttitude::Friendly)) continue;
        Actors.push_back(Enemy);
    }
    return Actors;
}

inline std::string ObjToStr(const UObject* Obj)
{
    if (!Obj) return "None";
    if (!IsValid(Obj)) return "None";
    return Obj->GetName();
}

template <typename T>
    requires IsAActor<T>
inline TArray<AActor*> GetAllActorsOfClass() {
    TArray<AActor*> Out;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), T::StaticClass(), &Out);
    return Out;
}

inline std::string parse_quoted(std::string_view input)
{
    std::string out;
    out.reserve(input.size());

    bool in_quotes = false;
    bool escape = false;
    bool found_quote = false;

    for (size_t i = 0; i < input.size(); ++i)
    {
        char c = input[i];

        if (!in_quotes)
        {
            if (c == '"')
            {
                in_quotes = true;
                found_quote = true;
            }
            continue;
        }

        if (escape)
        {
            switch (c)
            {
            case '"':  out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case 'n':  out.push_back('\n'); break;
            case 't':  out.push_back('\t'); break;
            default:   out.push_back(c); break; // unknown escape: keep literal
            }
            escape = false;
            continue;
        }

        if (c == '\\')
        {
            escape = true;
            continue;
        }

        if (c == '"')
        {
            return out;
        }

        out.push_back(c);
    }

    // If we never entered quotes -> return full string
    if (!found_quote)
        return std::string(input);

    // Unterminated quote -> return everything collected after opening quote
    return out;
}

// =========================================================================
// Numeric ranges
// =========================================================================

// ─────────────────────────────────────────────────────────────────────────────
// numeric_range<T>  —  C++23 step iterator for every primitive numeric type
//
//   Supported:
//     Integers : (signed/unsigned) char, short, int, long, long long
//     Floats   : float, double, long double
//              + __float16 / _Float16 if the compiler supports them
//
//   Construction:
//     numeric_range<T>(begin, end)         — step auto: +1 or -1 by direction
//     numeric_range<T>(begin, end, step)   — explicit step
//     range(begin, end)                    — factory, same auto-step rule
//     range(begin, end, step)              — factory, explicit step
//     range(end)                           — shorthand for [0, end)
//
//   Semantics:
//     Half-open [begin, end).  Stops as soon as the current value
//     would equal or pass end in the step direction.
//     step == 0 → std::invalid_argument at construction time.
// ─────────────────────────────────────────────────────────────────────────────

namespace nr_detail {

    template <typename T>
    concept NumericPrimitive =
        (std::integral<T>
            && !std::same_as<T, bool>
            && !std::same_as<T, wchar_t>
            && !std::same_as<T, char8_t>
            && !std::same_as<T, char16_t>
            && !std::same_as<T, char32_t>)
        || std::floating_point<T>
#if defined(__FLT16_MAX__)          // GCC/Clang native _Float16
        || std::same_as<T, _Float16>
#endif
        ;

    template <NumericPrimitive T>
    constexpr T default_step() noexcept { return static_cast<T>(1); }

} // namespace nr_detail


// ── Iterator ─────────────────────────────────────────────────────────────────
template <nr_detail::NumericPrimitive T>
class numeric_range_iterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T*;
    using reference = T;

    constexpr numeric_range_iterator(T cur, T end, T step) noexcept
        : cur_(cur), end_(end), step_(step) {
    }

    constexpr T         operator*()  const noexcept { return cur_; }
    constexpr pointer   operator->() const noexcept { return &cur_; }

    constexpr numeric_range_iterator& operator++() noexcept {
        cur_ = static_cast<T>(cur_ + step_);
        return *this;
    }
    constexpr numeric_range_iterator operator++(int) noexcept {
        auto c = *this; ++(*this); return c;
    }

    // Equality: "are we at or past end?"
    constexpr bool operator==(const numeric_range_iterator& o) const noexcept {
        return step_ > T{ 0 } ? cur_ >= o.cur_ : cur_ <= o.cur_;
    }
    constexpr bool operator!=(const numeric_range_iterator& o) const noexcept {
        return !(*this == o);
    }

private:
    T cur_, end_, step_;
};


// ── Range ─────────────────────────────────────────────────────────────────────
template <nr_detail::NumericPrimitive T>
class numeric_range {
public:
    using iterator = numeric_range_iterator<T>;

    // Two-arg: auto step ±1 based on direction
    constexpr numeric_range(T begin, T end)
        : begin_(begin), end_(end)
        , step_(end >= begin ? nr_detail::default_step<T>()
            : static_cast<T>(-1))
    {
    }

    // Three-arg: explicit step
    constexpr numeric_range(T begin, T end, T step)
        : begin_(begin), end_(end), step_(step)
    {
        if (step == T{ 0 })
            throw std::invalid_argument("numeric_range: step must not be zero");
    }

    constexpr iterator begin() const noexcept {
        if (step_ > T{ 0 } && begin_ >= end_) return sentinel();
        if (step_ < T{ 0 } && begin_ <= end_) return sentinel();
        return iterator{ begin_, end_, step_ };
    }
    constexpr iterator end() const noexcept { return sentinel(); }

    constexpr bool empty() const noexcept {
        if (step_ > T{ 0 }) return begin_ >= end_;
        if (step_ < T{ 0 }) return begin_ <= end_;
        return true;
    }

    // Integer-only exact size
    constexpr std::ptrdiff_t size() const noexcept
        requires std::integral<T>
    {
        if (step_ > T{ 0 } && end_ > begin_)
            return static_cast<std::ptrdiff_t>((end_ - begin_ + step_ - T{ 1 }) / step_);
        if (step_ < T{ 0 } && begin_ > end_)
            return static_cast<std::ptrdiff_t>((begin_ - end_ - step_ - T{ 1 }) / (-step_));
        return 0;
    }

private:
    constexpr iterator sentinel() const noexcept {
        return iterator{ end_, end_, step_ };
    }
    T begin_, end_, step_;
};

// CTAD
template <nr_detail::NumericPrimitive T>
numeric_range(T, T) -> numeric_range<T>;

template <nr_detail::NumericPrimitive T>
numeric_range(T, T, T) -> numeric_range<T>;


// ── Factory helpers ───────────────────────────────────────────────────────────
template <nr_detail::NumericPrimitive T>
constexpr auto range(T begin, T end) { return numeric_range<T>(begin, end); }

template <nr_detail::NumericPrimitive T>
constexpr auto range(T begin, T end, T step) { return numeric_range<T>(begin, end, step); }

template <nr_detail::NumericPrimitive T>
constexpr auto range(T end) { return numeric_range<T>(T{ 0 }, end); }

// ── Function flag diagnostics ─────────────────────────────────────────────
inline std::string DumpFunctionFlags(uint32_t flags)
{
    using F = SDK::EFunctionFlags;
    std::string out;
    auto add = [&](F f, const char* name) {
        if (flags & static_cast<uint32_t>(f)) {
            if (!out.empty()) out += '|';
            out += name;
        }
    };
    add(F::Final,                 "Final");
    add(F::RequiredAPI,           "ReqAPI");
    add(F::BlueprintAuthorityOnly,"BPAuthOnly");
    add(F::BlueprintCosmetic,     "BPCosmetic");
    add(F::Net,                   "Net");
    add(F::NetReliable,           "NetReliable");
    add(F::NetRequest,            "NetRequest");
    add(F::Exec,                  "Exec");
    add(F::Native,                "Native");
    add(F::Event,                 "Event");
    add(F::NetResponse,           "NetResponse");
    add(F::Static,                "Static");
    add(F::NetMulticast,          "NetMulticast");
    add(F::UbergraphFunction,     "UbergraphFn");
    add(F::MulticastDelegate,     "MCDelegate");
    add(F::Public,                "Public");
    add(F::Private,               "Private");
    add(F::Protected,             "Protected");
    add(F::Delegate,              "Delegate");
    add(F::NetServer,             "NetServer");
    add(F::HasOutParms,           "HasOutParms");
    add(F::HasDefaults,           "HasDefaults");
    add(F::NetClient,             "NetClient");
    add(F::DLLImport,             "DLLImport");
    add(F::BlueprintCallable,     "BPCallable");
    add(F::BlueprintEvent,        "BPEvent");
    add(F::BlueprintPure,         "BPPure");
    add(F::EditorOnly,            "EditorOnly");
    add(F::Const,                 "Const");
    add(F::NetValidate,           "NetValidate");
    if (out.empty()) out = "None";
    return out;
}