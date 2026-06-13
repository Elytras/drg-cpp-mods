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
#include <atomic>
#include <cstddef>             // std::max_align_t
#include <cstdint>
#include <cstdlib>             // std::abort

#ifndef __STDCPP_DEFAULT_NEW_ALIGNMENT__
#define __STDCPP_DEFAULT_NEW_ALIGNMENT__ 16
#endif

namespace
{
    // ── Bootstrap arena ──────────────────────────────────────────────────────────
    // Serves allocations that occur *while resolving* the allocator — i.e. a re-entrant
    // operator new before GMalloc is cached. The resolver is now allocation-free
    // (FindPatternNoAlloc), so in practice this is rarely if ever touched; it exists so a
    // future resolution-path allocation (or GMalloc simply not being up yet, e.g. a
    // CREATE_SUSPENDED inject) degrades to a tiny leak instead of infinite recursion or an
    // abort. Pointers here are range-detected in operator delete and never freed to GMalloc.
    alignas(std::max_align_t) unsigned char g_bootstrap[64 * 1024];
    std::atomic<std::size_t> g_bootstrapOff{0};
    thread_local int         g_inResolve = 0;   // >0 while this thread is resolving

    inline bool InBootstrap(void* p) noexcept
    {
        const auto a = reinterpret_cast<std::uintptr_t>(p);
        const auto b = reinterpret_cast<std::uintptr_t>(g_bootstrap);
        return a >= b && a < b + sizeof(g_bootstrap);
    }

    inline void* BootstrapAlloc(std::size_t size, std::size_t align) noexcept
    {
        if (align < 1) align = 1;
        std::size_t off = g_bootstrapOff.load(std::memory_order_relaxed), aligned, next;
        do {
            const auto base = reinterpret_cast<std::uintptr_t>(g_bootstrap) + off;
            const auto ali  = (base + (align - 1)) & ~static_cast<std::uintptr_t>(align - 1);
            aligned = off + (ali - base);
            next    = aligned + size;
            if (next > sizeof(g_bootstrap)) return nullptr;   // arena exhausted
        } while (!g_bootstrapOff.compare_exchange_weak(off, next, std::memory_order_relaxed));
        return g_bootstrap + aligned;
    }

    // Resolve the allocator, guarding against re-entrant allocation during resolution.
    // Fast path is a non-resolving Peek() so cached allocations never enter the guard.
    // Returns nullptr only if resolution failed AND we are not mid-resolution.
    inline UnrealAllocator* ResolveGuarded() noexcept
    {
        if (UnrealAllocator* c = UnrealAllocator::Peek()) return c;   // cached: no resolve, no guard
        if (g_inResolve) return nullptr;                              // re-entrant: caller uses bootstrap
        ++g_inResolve;
        UnrealAllocator* a = UnrealAllocator::Get();
        --g_inResolve;
        return a;
    }

    inline void* AllocOrThrow(std::size_t size, std::uint32_t align)
    {
        if (size == 0) size = 1;
        if (UnrealAllocator* a = ResolveGuarded())
        {
            if (void* p = a->Malloc(size, align)) return p;
            throw std::bad_alloc();
        }
        // Allocator unavailable (resolving, or genuinely not up) → bootstrap. Never fall back
        // to the CRT: a CRT pointer freed via GMalloc later would corrupt.
        if (void* p = BootstrapAlloc(size, align)) return p;
        throw std::bad_alloc();
    }

    inline void* AllocNoThrow(std::size_t size, std::uint32_t align) noexcept
    {
        if (size == 0) size = 1;
        if (UnrealAllocator* a = ResolveGuarded())
            return a->TryMalloc(size, align);   // never throws; returns null on OOM
        return BootstrapAlloc(size, align);
    }

    inline void FreePtr(void* p) noexcept
    {
        if (!p) return;
        if (InBootstrap(p)) return;   // arena allocation — never returned to GMalloc
        // Free is reachable on shutdown paths too; if the allocator vanished there is
        // nothing safe to do, so swallow rather than abort. Peek (no resolve): a real
        // GMalloc pointer can only exist if the allocator was already resolved.
        if (UnrealAllocator* a = UnrealAllocator::Peek())
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
