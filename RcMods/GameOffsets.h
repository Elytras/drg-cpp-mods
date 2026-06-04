#pragma once
// GameOffsets.h (RC / UE 5.6) — per-game RVAs, signatures, and vtable indices.
// All values are determined empirically from RogueCore-Win64-Shipping.exe.

#include <cstdint>
#include <string_view>

namespace GameOffsets
{
    // ── StaticConstructObject_Internal ──────────────────────────────────────
    //
    // TBD — disassemble a NewObject<T> call site in RogueCore-Win64-Shipping.exe
    // to find the SCO_Internal address, then either:
    //   (a) record the RVA below (preferred — survives ASLR, fast at runtime), or
    //   (b) build a call-site signature like DRG uses (slower but more robust to
    //       game patches).
    //
    // While both are zero/empty, Lib_ObjectFactory::GetStaticConstructObject()
    // returns nullptr and NewObject<T>() returns nullptr (logs once on first
    // attempt).
    inline constexpr uintptr_t       kStaticConstructObjectRVA     = 0;
    inline constexpr std::string_view kStaticConstructObjectCallSig = "";
}
