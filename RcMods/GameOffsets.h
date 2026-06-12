#pragma once
// GameOffsets.h (RC / UE 5.6) — per-game RVAs, signatures, and vtable indices.
// All values are determined empirically from RogueCore-Win64-Shipping.exe.

#include <cstdint>
#include <string_view>

namespace GameOffsets
{
    // ── StaticConstructObject_Internal ──────────────────────────────────────
    // RVA from Dumper-7 SDK::Offsets::StaticConstructObjectInternal.
    // Confirmed from 5.6.1-145115+main-RogueCore (experimental branch).
    // Update this whenever the SDK is regenerated.
    inline constexpr uintptr_t       kStaticConstructObjectRVA     = 0x015DBB90;
    inline constexpr std::string_view kStaticConstructObjectCallSig = "";
}
