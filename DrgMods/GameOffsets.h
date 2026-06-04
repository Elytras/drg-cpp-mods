#pragma once
// GameOffsets.h (DRG / UE 4.27) — per-game RVAs, signatures, and vtable indices.
// All values are determined empirically from FSD-Win64-Shipping.exe disassembly.
// Update when the game patches or the executable changes structure.

#include <cstdint>
#include <string_view>

namespace GameOffsets
{
    // ── StaticConstructObject_Internal ──────────────────────────────────────
    //
    // RVA confirmed from FSD-Win64-Shipping.exe via the NewObject<UEngine>
    // call site @ 140859422. Stable across game patches that don't reshape
    // CoreUObject.
    inline constexpr uintptr_t kStaticConstructObjectRVA = 0x1F9C870;

    // Call-site signature: the E8 call to StaticConstructObject_Internal
    // immediately before the NewObject<UEngine> epilogue. Used as a fallback
    // when the RVA above is stale (e.g. after a UE engine update).
    inline constexpr std::string_view kStaticConstructObjectCallSig =
        "E8 ?? ?? ?? ?? "
        "48 8B 5C 24 70 "
        "48 8B 6C 24 78 "
        "48 8B B4 24 88 00 00 00 "
        "48 83 C4 60 "
        "5F C3";
}
