#pragma once
// Lib_ObjectCast.h — UObject cast system, class flag helpers, IsValidRaw.

#include <cassert>
#include "Lib_Forward.h"

namespace ObjectCast
{
    inline bool HasFlag(const UObject* Obj, EClassCastFlags Flag)
    {
        if (!Obj || !Obj->Class) return false;
        return (static_cast<uint64>(Obj->Class->CastFlags) & static_cast<uint64>(Flag)) != 0;
    }

    template<typename T>
    T* Cast(UObject* Obj)
    {
        return (Obj && Obj->IsA(T::StaticClass())) ? static_cast<T*>(Obj) : nullptr;
    }

    template<typename T>
    T* CastChecked(UObject* Obj)
    {
        assert(Obj && Obj->IsA(T::StaticClass()));
        return static_cast<T*>(Obj);
    }
} // namespace ObjectCast

// ClassFlags at 0x00CC (before CastFlags at 0x00D0)
inline EClassFlags GetClassFlags(const UClass* Class)
{
    return *reinterpret_cast<const EClassFlags*>(
        reinterpret_cast<const uintptr_t>(Class) + 0x00CC);
}

inline bool HasClassFlag(const UClass* Class, EClassFlags Flag)
{
    return (static_cast<uint32>(GetClassFlags(Class)) & static_cast<uint32>(Flag)) != 0;
}

inline bool IsNativeClass(const UClass* Class) { return Class && HasClassFlag(Class, EClassFlags::Native); }
inline bool IsBlueprintClass(const UClass* Class) { return Class && HasClassFlag(Class, EClassFlags::CompiledFromBlueprint); }

inline bool IsValidRaw(const UObject* Obj)
{
    if (!Obj) return false;
    constexpr int32 GarbageFlags =
        (int32)EObjectFlags::BeginDestroyed |
        (int32)EObjectFlags::MirroredGarbage;
    return !(Obj->Flags & (EObjectFlags)GarbageFlags);
}

struct UClassHierarchyRange
{
    explicit UClassHierarchyRange(UClass* start) : start_(start) {}

    struct Iterator
    {
        using iterator_category = std::forward_iterator_tag;
        using value_type = UClass;
        using pointer = UClass*;
        using reference = UClass&;
        using difference_type = std::ptrdiff_t;

        explicit Iterator(UClass* c) : current_(c) {}
        pointer   operator*()  const { return current_; }
        Iterator& operator++() { current_ = ObjectCast::Cast<UClass>(current_->SuperStruct); return *this; }
        Iterator  operator++(int) { auto t = *this; ++(*this); return t; }
        bool operator==(const Iterator& o) const { return current_ == o.current_; }
        bool operator!=(const Iterator& o) const { return current_ != o.current_; }
    private:
        UClass* current_;
    };

    Iterator begin() const { return Iterator(start_); }
    Iterator end()   const { return Iterator(nullptr); }
private:
    UClass* start_;
};

inline bool IsChildOfByName(const UObject* object, const std::wstring& name)
{
    const FName target(name.c_str());
    for (UClass* c : UClassHierarchyRange(object->Class))
        if (c->Name == target) return true;
    return false;
}