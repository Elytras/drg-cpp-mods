#include "Lib_ObjectFactory.h"

namespace ObjectFactory
{
    FStaticConstructObjectFn GetStaticConstructObject()
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
} // namespace ObjectFactory
