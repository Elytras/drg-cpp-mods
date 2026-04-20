#pragma once
// Library.h — Single include for all DRG mod utilities.
// Order matters: each header depends on those above it.

#define EXTRA_LEAN

// ─── STL ─────────────────────────────────────────────────────────────────────
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// ─── Third-party ──────────────────────────────────────────────────────────────
#include <MinHook.h>
#include <spdlog/spdlog.h>

// ─── Internal base ────────────────────────────────────────────────────────────
#include "Common.h"
#include "StringLib.h"
#include "SDK/UtfN.hpp"
#include "SDK/SDK/Basic.hpp"
#include "SDK/SDK/Engine_classes.hpp"
#include "SDK/SDK/FSD_classes.hpp"
#include "SDK/SDK/AssetRegistry_classes.hpp"
#include "SDK/SDK/AssetRegistry_structs.hpp"

#ifndef CatchFuncs
#define CatchFuncs 0
#endif
#ifndef CheckWorld
#define CheckWorld 0
#endif
#ifdef min
#undef min
#endif

// FName hash — needed before GameHooks
template<>
struct std::hash<SDK::FName>
{
    size_t operator()(const SDK::FName& n) const noexcept
    {
        return std::hash<uint64_t>{}(
            (uint64_t)(uint32_t)n.ComparisonIndex << 32 | n.Number);
    }
};
// Listed in dependency order for ease of use. Each header should be self-contained and only depend on those above it.
#include "Lib_Forward.h"            // namespace/type aliases, forward decls
#include "Lib_FField.h"             // FField types, FieldCast, iterators
#include "Lib_ObjectCast.h"         // ObjectCast, class flags, IsValidRaw
#include "Lib_PropertyAccess.h"     // GetPropertyPtr, ReadBool/WriteBool, GetTypeName
#include "Lib_Utils.h"              // SubclassCache, safe parsers, GetLocalPlayer, etc.
#include "Lib_Print.h"              // PrintFieldValue, DumpItemProperties, Detail
#include "Lib_EasyHook.h"           // MinHook wrapper
#include "Lib_GameHooks.h"          // ProcessEventHook, GameHooks API
#include "Lib_VarSystem.h"          // VarSystem (commands deferred to Lib_CommandHandler.h)
#include "Lib_CommandHandler.h"     // CommandContext, CommandHandler, VarSystem commands
#include "Lib_PropertyInspector.h"  // PropertyInspector namespace