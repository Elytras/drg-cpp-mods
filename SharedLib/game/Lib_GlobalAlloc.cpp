// Lib_GlobalAlloc.cpp — route ALL module C++ allocations through the game's
// UnrealAllocator (GMalloc), per ROADMAP §1.
//
// MSVC per-module operator-new/delete replacement only affects THIS DLL, so
// every std/spdlog/yaml-cpp/json/mod-code `new`/`delete` lands in GMalloc while
// other modules (and the CRT's own `malloc`) are untouched. This kills the
// cross-allocator bug class and lets data handed to the engine survive a
// hot-reload unload.
//
// IMPORTANT — this is a BUILD-TIME switch, not a runtime toggle. Every `delete`
// must match the backend that allocated it, so the whole TU is gated on
// UE_GLOBAL_ALLOC (set per-project in the .vcxproj). When the flag is 0/unset
// this TU is empty and the CRT's default operators link in unchanged — zero
// call-site changes either way. CRT `malloc`/`free` are never redirected here.
//
// Fail-fast policy: if the allocator can't be resolved we std::abort rather than
// silently fall back to CRT (a CRT-allocated pointer freed via GMalloc — or the
// reverse — is undefined behavior). By the time our DLL's static init runs on the
// normal injection path GMalloc already exists; on a future CREATE_SUSPENDED path
// (§5) UnrealAllocator::Resolve() drives CreateGMalloc itself before ResumeThread.

#if defined(UE_GLOBAL_ALLOC) && UE_GLOBAL_ALLOC

#include "UnrealCoreTypes.h"   // UnrealAllocator
#include <new>
#include <cstdlib>             // std::abort

#ifndef __STDCPP_DEFAULT_NEW_ALIGNMENT__
#define __STDCPP_DEFAULT_NEW_ALIGNMENT__ 16
#endif

namespace
{
    // The allocator is mandatory once this TU is enabled. If it cannot be
    // resolved there is no safe backend to allocate from, so abort rather than
    // return a CRT pointer that a later GMalloc-backed delete would corrupt on.
    inline UnrealAllocator* RequireAllocator()
    {
        UnrealAllocator* a = UnrealAllocator::Get();
        if (!a) std::abort();
        return a;
    }

    inline void* AllocOrThrow(std::size_t size, std::uint32_t align)
    {
        if (size == 0) size = 1;
        void* p = RequireAllocator()->Malloc(size, align);
        if (!p) throw std::bad_alloc();
        return p;
    }

    inline void* AllocNoThrow(std::size_t size, std::uint32_t align) noexcept
    {
        if (size == 0) size = 1;
        UnrealAllocator* a = UnrealAllocator::Get();
        if (!a) return nullptr;
        return a->TryMalloc(size, align);   // never throws; returns null on OOM
    }

    inline void FreePtr(void* p) noexcept
    {
        if (!p) return;
        // Free is reachable on shutdown paths too; if the allocator vanished
        // there is nothing safe to do, so swallow rather than abort.
        if (UnrealAllocator* a = UnrealAllocator::Get())
            a->Free(p);
    }
}

// ── throwing new ──────────────────────────────────────────────────────────────
void* operator new  (std::size_t size)                 { return AllocOrThrow(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }
void* operator new[](std::size_t size)                 { return AllocOrThrow(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }
void* operator new  (std::size_t size, std::align_val_t al) { return AllocOrThrow(size, static_cast<std::uint32_t>(al)); }
void* operator new[](std::size_t size, std::align_val_t al) { return AllocOrThrow(size, static_cast<std::uint32_t>(al)); }

// ── nothrow new ────────────────────────────────────────────────────────────────
void* operator new  (std::size_t size, const std::nothrow_t&) noexcept { return AllocNoThrow(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return AllocNoThrow(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }
void* operator new  (std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept { return AllocNoThrow(size, static_cast<std::uint32_t>(al)); }
void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept { return AllocNoThrow(size, static_cast<std::uint32_t>(al)); }

// ── delete (plain / sized / aligned / sized+aligned / nothrow) ──────────────────
// UnrealAllocator::Free needs neither size nor alignment, so every form forwards
// to FreePtr; the extra params exist only so the compiler picks our operator over
// the CRT default for sized/aligned deallocation call sites.
void operator delete  (void* p) noexcept { FreePtr(p); }
void operator delete[](void* p) noexcept { FreePtr(p); }
void operator delete  (void* p, std::size_t) noexcept { FreePtr(p); }
void operator delete[](void* p, std::size_t) noexcept { FreePtr(p); }
void operator delete  (void* p, std::align_val_t) noexcept { FreePtr(p); }
void operator delete[](void* p, std::align_val_t) noexcept { FreePtr(p); }
void operator delete  (void* p, std::size_t, std::align_val_t) noexcept { FreePtr(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { FreePtr(p); }
void operator delete  (void* p, const std::nothrow_t&) noexcept { FreePtr(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { FreePtr(p); }
void operator delete  (void* p, std::align_val_t, const std::nothrow_t&) noexcept { FreePtr(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { FreePtr(p); }

#endif // UE_GLOBAL_ALLOC
