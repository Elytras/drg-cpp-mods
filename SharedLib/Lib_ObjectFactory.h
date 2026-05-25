#pragma once
// Lib_ObjectFactory.h — StaticConstructObject_Internal lookup and NewObject wrapper.

#include <cstdint>
#include <string_view>
#include "Lib_Forward.h"
#include "Lib_Utils.h"

// =========================================================================
// EInternalObjectFlags
// =========================================================================

enum EInternalObjectFlags : uint32_t
{
    IOF_None = 0x00000000,
    IOF_ReachableInCluster = 0x00800000,
    IOF_ClusterRoot = 0x01000000,
    IOF_Native = 0x02000000,
    IOF_Async = 0x04000000,
    IOF_AsyncLoading = 0x08000000,
    IOF_Unreachable = 0x10000000,
    IOF_PendingKill = 0x20000000,
    IOF_RootSet = 0x40000000,
    IOF_PendingConstruction = 0x80000000,
    IOF_GarbageCollectionKeepFlags = 0x0E000000,
    IOF_AllFlags = 0xFF800000,
};

// =========================================================================
// FFrame — minimal UE4.27 script call frame
// Layout: vtptr+bools+pad = 0x10, Node = 0x10, Object = 0x18,
//         Code = 0x20, Locals = 0x28.  (FlowStack at 0x40, etc. omitted.)
// =========================================================================

struct FFrame
{
    uint8_t       Pad00[0x10]; // vtptr + boolean fields
    void*         Node;        // 0x10 — UFunction* being executed
    SDK::UObject* Object;      // 0x18 — context UObject*
    uint8_t*      Code;        // 0x20 — current bytecode pointer
    uint8_t*      Locals;      // 0x28 — parameter / locals buffer
};
static_assert(offsetof(FFrame, Node)   == 0x10);
static_assert(offsetof(FFrame, Object) == 0x18);
static_assert(offsetof(FFrame, Code)   == 0x20);
static_assert(offsetof(FFrame, Locals) == 0x28);

// =========================================================================
// FStaticConstructObjectParameters
// Layout confirmed from FSD-Win64-Shipping.exe disassembly @ 141f9c840
// =========================================================================

struct FStaticConstructObjectParameters
{
    SDK::UClass const*            Class;
    SDK::UObject*                 Outer;
    SDK::FName                    Name;
    SDK::EObjectFlags             SetFlags;
    enum EInternalObjectFlags     InternalSetFlags;
    bool                          bCopyTransientsFromClassDefaults;
    bool                          bAssumeTemplateIsArchetype;
    SDK::UObject*                 Template;
    class FObjectInstancingGraph* InstanceGraph;
    SDK::UPackage*                ExternalPackage;
};

static_assert(offsetof(FStaticConstructObjectParameters, Name) == 0x10);
static_assert(offsetof(FStaticConstructObjectParameters, SetFlags) == 0x18);
static_assert(offsetof(FStaticConstructObjectParameters, InternalSetFlags) == 0x1C);
static_assert(offsetof(FStaticConstructObjectParameters, bCopyTransientsFromClassDefaults) == 0x20);
static_assert(offsetof(FStaticConstructObjectParameters, bAssumeTemplateIsArchetype) == 0x21);
static_assert(offsetof(FStaticConstructObjectParameters, Template) == 0x28);

// =========================================================================
// ObjectFactory
// =========================================================================

namespace ObjectFactory
{
    using namespace SDK;

    using FStaticConstructObjectFn = UObject * (*)(FStaticConstructObjectParameters*);

    // RVA + call-site signature live in per-game GameOffsets.h:
    //   DrgMods/GameOffsets.h — DRG (UE 4.27, FSD-Win64-Shipping.exe)
    //   RcMods/GameOffsets.h  — RC  (UE 5.6,  RogueCore-Win64-Shipping.exe) — TBD
    //
    // Returns nullptr (and logs once) if neither resolution path succeeds.
    FStaticConstructObjectFn GetStaticConstructObject();

    // =========================================================================
    // NewObject<T>
    // =========================================================================

    template<typename T = UObject>
        requires IsUObject<T>
    inline T* NewObject(
        UObject*                Outer = nullptr,
        UClass*                 Class = nullptr,
        const wchar_t*          Name = L"",
        EObjectFlags            Flags = EObjectFlags::NoFlags,
        UObject*                Template = nullptr,
        EInternalObjectFlags    Internal = IOF_None)
    {
        auto* fn = GetStaticConstructObject();
        if (!fn) return nullptr;

        FStaticConstructObjectParameters p{};
        p.Class = Class ? Class : T::StaticClass();
        p.Outer = Outer;
        p.Name = FName(Name);
        p.SetFlags = Flags;
        p.InternalSetFlags = Internal;
        p.Template = Template;

        return ObjectCast::CastChecked<T>(fn(&p));
    }

    template<typename T = UObject>
        requires IsUObject<T>
    inline T* NewObject(const FStaticConstructObjectParameters& Params) {
        auto* fn = GetStaticConstructObject();
        if (!fn) return nullptr;
        return ObjectCast::CastChecked<T>(fn(const_cast<FStaticConstructObjectParameters*>(&Params)));
    }
} // namespace ObjectFactory
