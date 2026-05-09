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
    uint8_t  Pad00[0x10]; // vtptr + boolean fields
    void*    Node;        // 0x10 — UFunction* being executed
    UObject* Object;      // 0x18 — context UObject*
    uint8_t* Code;        // 0x20 — current bytecode pointer
    uint8_t* Locals;      // 0x28 — parameter / locals buffer
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
    class UClass const* Class;
    class UObject* Outer;
    class FName Name;
    enum EObjectFlags SetFlags;
    enum EInternalObjectFlags InternalSetFlags;
    bool bCopyTransientsFromClassDefaults;
    bool bAssumeTemplateIsArchetype;
    class UObject* Template;
    class FObjectInstancingGraph* InstanceGraph;
    class UPackage* ExternalPackage;
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
    using FStaticConstructObjectFn = UObject * (*)(FStaticConstructObjectParameters*);

    // RVA confirmed from FSD-Win64-Shipping.exe disassembly (NewObject<UEngine> call site @ 140859422).
    static constexpr uintptr_t kStaticConstructObjectRVA = 0x1F9C870;

    // Call-site signature: the E8 call to StaticConstructObject_Internal immediately before
    // the NewObject<UEngine> epilogue. Stable because the epilogue layout (callee-save restore
    // order and rsp offsets) is determined by the MSVC ABI and the function's frame size, not
    // by game logic — changes only if the UEngine template instantiation's frame layout changes.
    //
    // Source: NewObject<UEngine> @ 140859380, call @ 140859422, epilogue @ 140859427:
    //   E8 ?? ?? ?? ??          call  StaticConstructObject_Internal
    //   48 8B 5C 24 70          mov   rbx, [rsp+0x70]
    //   48 8B 6C 24 78          mov   rbp, [rsp+0x78]
    //   48 8B B4 24 88 00 00 00 mov   rsi, [rsp+0x88]
    //   48 83 C4 60             add   rsp, 0x60
    //   5F C3                   pop   rdi; retn
    static constexpr std::string_view kStaticConstructObjectCallSig =
        "E8 ?? ?? ?? ?? "
        "48 8B 5C 24 70 "
        "48 8B 6C 24 78 "
        "48 8B B4 24 88 00 00 00 "
        "48 83 C4 60 "
        "5F C3";

    inline FStaticConstructObjectFn GetStaticConstructObject()
    {
        static FStaticConstructObjectFn fn = nullptr;
        if (fn) return fn;

        // Try static RVA first — fast, version-locked. Skip when RVA is 0 (unknown).
        if constexpr (kStaticConstructObjectRVA != 0)
        {
            HMODULE mod = GetModuleHandleW(L"FSD-Win64-Shipping.exe");
            if (mod)
            {
                auto* candidate = reinterpret_cast<uint8_t*>(mod) + kStaticConstructObjectRVA;
                if (candidate[0] != 0xCC && candidate[0] != 0x00)
                {
                    fn = reinterpret_cast<FStaticConstructObjectFn>(candidate);
                    spdlog::debug("[ObjectFactory] StaticConstructObject_Internal: RVA hit @ {:p}",
                        reinterpret_cast<void*>(fn));
                    return fn;
                }
                spdlog::warn("[ObjectFactory] StaticConstructObject_Internal: RVA looks invalid (0x{:02X}), falling back to call-site scan",
                    candidate[0]);
            }
        }

        // Fall back: find the call instruction immediately before NewObject<UEngine>'s epilogue,
        // then decode its relative offset to get StaticConstructObject_Internal.
        auto* callSite = reinterpret_cast<uint8_t*>(
            FindPattern(L"FSD-Win64-Shipping.exe", kStaticConstructObjectCallSig));
        if (callSite)
        {
            int32_t rel = *reinterpret_cast<int32_t*>(callSite + 1);
            fn = reinterpret_cast<FStaticConstructObjectFn>(callSite + 5 + rel);
            spdlog::debug("[ObjectFactory] StaticConstructObject_Internal: call-site scan hit @ {:p}",
                reinterpret_cast<void*>(fn));
            return fn;
        }

        spdlog::error("[ObjectFactory] StaticConstructObject_Internal: both RVA and call-site scan failed");
        return fn;
    }

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
