#include "Lib_ObjectFactory.h"
#include "Common.h"        // TARGET_PROCESS
#include "GameOffsets.h"   // kStaticConstructObjectRVA, …CallSig

namespace ObjectFactory
{
    FStaticConstructObjectFn GetStaticConstructObject()
    {
        static FStaticConstructObjectFn fn = nullptr;
        static bool warnedUnresolved = false;
        if (fn) return fn;

        // Fast path: static RVA. Skip when RVA is 0 (unconfigured for this
        // game — e.g. RC until someone supplies one from disassembly).
        if constexpr (GameOffsets::kStaticConstructObjectRVA != 0)
        {
            HMODULE mod = GetModuleHandleW(TARGET_PROCESS);
            if (mod)
            {
                auto* candidate = reinterpret_cast<uint8_t*>(mod)
                                + GameOffsets::kStaticConstructObjectRVA;
                if (candidate[0] != 0xCC && candidate[0] != 0x00)
                {
                    fn = reinterpret_cast<FStaticConstructObjectFn>(candidate);
                    debug("[ObjectFactory] SCO_Internal: RVA hit @ {:p}",
                        reinterpret_cast<void*>(fn));
                    return fn;
                }
                warn("[ObjectFactory] SCO_Internal: RVA looks invalid (0x{:02X}), trying call-site scan",
                    candidate[0]);
            }
        }

        // Fallback: scan for the call-site signature. Empty signature means
        // this game has no scan pattern yet — skip the scan and report.
        if constexpr (!GameOffsets::kStaticConstructObjectCallSig.empty())
        {
            auto* callSite = reinterpret_cast<uint8_t*>(
                FindPattern(TARGET_PROCESS, GameOffsets::kStaticConstructObjectCallSig));
            if (callSite)
            {
                int32_t rel = *reinterpret_cast<int32_t*>(callSite + 1);
                fn = reinterpret_cast<FStaticConstructObjectFn>(callSite + 5 + rel);
                debug("[ObjectFactory] SCO_Internal: call-site scan hit @ {:p}",
                    reinterpret_cast<void*>(fn));
                return fn;
            }
        }

        if (!warnedUnresolved)
        {
            warnedUnresolved = true;
            error("[ObjectFactory] SCO_Internal unresolved — NewObject<T>() will return nullptr. "
                  "Populate GameOffsets::kStaticConstructObjectRVA or …CallSig for this build.");
        }
        return nullptr;
    }
} // namespace ObjectFactory
