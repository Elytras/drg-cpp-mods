#pragma once
// Lib_ObjectCast.h � UObject cast system, class flag helpers, IsValidRaw.

#include "Lib_Forward.h"

namespace ObjectCast
{
    inline bool HasFlag(const SDK::UObject* Obj, SDK::EClassCastFlags Flag)
    {
        if (!Obj || !Obj->Class) return false;
        return (static_cast<uint64>(Obj->Class->CastFlags) & static_cast<uint64>(Flag)) != 0;
    }

    template<typename T>
    T* Cast(SDK::UObject* Obj)
    {
        return (Obj && Obj->IsA(T::StaticClass())) ? static_cast<T*>(Obj) : nullptr;
    }

    template<typename T>
    T* CastChecked(SDK::UObject* Obj)
    {
        assert(Obj && Obj->IsA(T::StaticClass()));
        return static_cast<T*>(Obj);
    }
} // namespace ObjectCast

// ClassFlags at 0x00CC (before CastFlags at 0x00D0)
SDK::EClassFlags GetClassFlags   (const SDK::UClass* Class);
bool             HasClassFlag    (const SDK::UClass* Class, SDK::EClassFlags Flag);
bool             IsNativeClass   (const SDK::UClass* Class);
bool             IsBlueprintClass(const SDK::UClass* Class);
bool             IsValidRaw      (const SDK::UObject* Obj);

struct UClassHierarchyRange
{
    explicit UClassHierarchyRange(SDK::UClass* start) : start_(start) {}

    struct Iterator
    {
        using iterator_category = std::forward_iterator_tag;
        using value_type = SDK::UClass;
        using pointer = SDK::UClass*;
        using reference = SDK::UClass&;
        using difference_type = std::ptrdiff_t;

        explicit Iterator(SDK::UClass* c) : current_(c) {}
        pointer   operator*()  const { return current_; }
        Iterator& operator++() { current_ = ObjectCast::Cast<SDK::UClass>(current_->SuperStruct); return *this; }
        Iterator  operator++(int) { auto t = *this; ++(*this); return t; }
        bool operator==(const Iterator& o) const { return current_ == o.current_; }
        bool operator!=(const Iterator& o) const { return current_ != o.current_; }
    private:
        SDK::UClass* current_;
    };

    Iterator begin() const { return Iterator(start_); }
    Iterator end()   const { return Iterator(nullptr); }
private:
    SDK::UClass* start_;
};

// =========================================================================
// GObjects range  —  for (auto* obj : GObjectsOf<T>()) { ... }
//
// Iterates the global UObject table, skipping null and invalid entries.
// When T != UObject the iterator also filters by IsA(T::StaticClass()).
// Count is snapshotted at construction; objects added mid-loop are ignored.
// =========================================================================

template<typename T = SDK::UObject>
class GObjectsRange
{
    int32 num_;

    static bool Matches(SDK::UObject* obj)
    {
        if (!obj || !Kismet::IsValid(obj)) return false;
        if constexpr (std::is_same_v<T, SDK::UObject>)
            return true;
        else
            return obj->IsA(T::StaticClass());
    }

public:
    class iterator
    {
        int32 idx_;
        int32 num_;

        void Advance()
        {
            while (idx_ < num_ && !GObjectsRange::Matches(SDK::UObject::GObjects->GetByIndex(idx_)))
                ++idx_;
        }

    public:
        using iterator_category = std::input_iterator_tag;
        using value_type        = T*;
        using difference_type   = std::ptrdiff_t;
        using pointer           = T*;
        using reference         = T*;

        iterator(int32 idx, int32 num) : idx_(idx), num_(num) { Advance(); }

        T*        operator*()  const { return static_cast<T*>(SDK::UObject::GObjects->GetByIndex(idx_)); }
        iterator& operator++()       { ++idx_; Advance(); return *this; }
        iterator  operator++(int)    { auto c = *this; ++(*this); return c; }

        bool operator==(const iterator& o) const { return idx_ == o.idx_; }
        bool operator!=(const iterator& o) const { return idx_ != o.idx_; }
    };

    GObjectsRange() : num_(SDK::UObject::GObjects ? SDK::UObject::GObjects->Num() : 0) {}

    iterator begin() const { return { 0,    num_ }; }
    iterator end()   const { return { num_, num_ }; }
};

template<typename T = SDK::UObject>
inline GObjectsRange<T> GObjectsOf() { return {}; }

bool IsChildOfByName(const SDK::UObject* object, const std::wstring& name);
bool IsChildOfByName(const SDK::UObject* object, const SDK::FName& name);