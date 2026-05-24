#include "Lib_ObjectCast.h"

using namespace SDK;  // file-local; no math types used in this TU

// ClassFlags at 0x00CC (before CastFlags at 0x00D0)
EClassFlags GetClassFlags(const UClass* Class)
{
    return *reinterpret_cast<const EClassFlags*>(
        reinterpret_cast<const uintptr_t>(Class) + 0x00CC);
}

bool HasClassFlag(const UClass* Class, EClassFlags Flag)
{
    return (static_cast<uint32>(GetClassFlags(Class)) & static_cast<uint32>(Flag)) != 0;
}

bool IsNativeClass   (const UClass* Class) { return Class && HasClassFlag(Class, EClassFlags::Native); }
bool IsBlueprintClass(const UClass* Class) { return Class && HasClassFlag(Class, EClassFlags::CompiledFromBlueprint); }

bool IsValidRaw(const UObject* Obj)
{
    if (!Obj) return false;
    constexpr int32 GarbageFlags =
        (int32)EObjectFlags::BeginDestroyed |
        (int32)EObjectFlags::MirroredGarbage;
    return !(Obj->Flags & (EObjectFlags)GarbageFlags);
}

bool IsChildOfByName(const UObject* object, const std::wstring& name)
{
    const FName target(name.c_str());
    for (UClass* c : UClassHierarchyRange(object->Class))
        if (c->Name == target) return true;
    return false;
}

bool IsChildOfByName(const UObject* object, const FName& name)
{
    for (UClass* c : UClassHierarchyRange(object->Class))
        if (c->Name == name) return true;
    return false;
}