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
// FStaticConstructObjectParameters
// Layout confirmed from FSD-Win64-Shipping.exe disassembly @ 141f9c840
// =========================================================================

struct FStaticConstructObjectParameters
{
    struct UClass const* Class;
    struct UObject* Outer;
    struct FName Name;
    enum EObjectFlags SetFlags;
    enum EInternalObjectFlags InternalSetFlags;
    bool bCopyTransientsFromClassDefaults;
    bool bAssumeTemplateIsArchetype;
    struct UObject* Template;
    struct FObjectInstancingGraph* InstanceGraph;
    struct UPackage* ExternalPackage;
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

    // RVA from FSD-Win64-Shipping.exe @ 0x141f9c840 (base 0x140000000)
    static constexpr uintptr_t kStaticConstructObjectRVA = 0x1f9c840;

    // Signature derived from FSD-Win64-Shipping.exe @ 0x141f9c840.
    // Wildcards on: __security_cookie rip-relative, lea r12 rip-relative.
    static constexpr std::string_view kStaticConstructObjectSig =
        "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 "
        "48 8D AC 24 50 FF FF FF 48 81 EC B0 01 00 00 "
        "48 8B 05 ?? ?? ?? ?? 48 33 C4 "
        "48 89 85 A8 00 00 00 48 8B 39 "
        "4C 8D 25 ?? ?? ?? ?? "
        "4C 8B 79 08 48 8B D9 8B 71 18 4C 8B 71 28 "
        "F7 87 CC 00 00 00 80 00 10 00";

    inline FStaticConstructObjectFn GetStaticConstructObject()
    {
        static FStaticConstructObjectFn fn = nullptr;
        if (fn) return fn;

        // Try static RVA first — fast, version-locked
        HMODULE mod = GetModuleHandleW(L"FSD-Win64-Shipping.exe");
        if (mod)
        {
            auto* candidate = reinterpret_cast<uint8_t*>(mod) + kStaticConstructObjectRVA;
            // Validate first 4 bytes of prologue before trusting the RVA
            if (candidate[0] == 0x48 && candidate[1] == 0x89 &&
                candidate[2] == 0x5C && candidate[3] == 0x24)
            {
                fn = reinterpret_cast<FStaticConstructObjectFn>(candidate);
                spdlog::debug("[ObjectFactory] StaticConstructObject_Internal: RVA hit @ {:p}",
                    reinterpret_cast<void*>(fn));
                return fn;
            }
            spdlog::warn("[ObjectFactory] StaticConstructObject_Internal: RVA prologue mismatch, falling back to scan");
        }

        fn = reinterpret_cast<FStaticConstructObjectFn>(
            FindPattern(L"FSD-Win64-Shipping.exe", kStaticConstructObjectSig));

        if (!fn)
            spdlog::error("[ObjectFactory] StaticConstructObject_Internal: pattern not found");
        else
            spdlog::debug("[ObjectFactory] StaticConstructObject_Internal: pattern hit @ {:p}",
                reinterpret_cast<void*>(fn));

        return fn;
    }

    // =========================================================================
    // NewObject<T>
    // =========================================================================

    template<typename T = UObject>
        requires IsUObject<T>
    inline T* NewObject(
        UObject* Outer = nullptr,
        UClass* Class = nullptr,
        FName                Name = FName{},
        EObjectFlags         Flags = EObjectFlags::NoFlags,
        UObject* Template = nullptr,
        EInternalObjectFlags Internal = IOF_None)
    {
        auto* fn = GetStaticConstructObject();
        if (!fn) return nullptr;

        FStaticConstructObjectParameters p{};
        p.Class = Class ? Class : T::StaticClass();
        p.Outer = Outer;
        p.Name = Name;
        p.SetFlags = Flags;
        p.InternalSetFlags = Internal;
        p.Template = Template;

        return static_cast<T*>(fn(&p));
    }

} // namespace ObjectFactory
