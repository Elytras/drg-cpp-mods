#pragma once
// Library.h — Single include for all RC mod utilities.
// Order matters: each header depends on those above it.

#ifndef EXTRA_LEAN
#define EXTRA_LEAN
#endif

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
#include "SDK/SDK/CoreUObject_classes.hpp"
#include "SDK/SDK/RogueCore_classes.hpp"

#ifndef CatchFuncs
#define CatchFuncs 0
#endif
#ifndef CheckWorld
#define CheckWorld 0
#endif
#ifdef min
#undef min
#endif

// Listed in dependency order for ease of use. Each header should be self-contained and only depend on those above it.
#include "Lib_Forward.h"            // namespace/type aliases, forward decls
//#include "Lib_VTableInfo.h"         // VTable index info for unreal functions
#include "../SharedLib/Lib_ObjectCast.h"         // ObjectCast, class flags, IsValidRaw
#include "Lib_PropertyAccess.h"     // GetPropertyPtr, ReadBool/WriteBool, GetTypeName
#include "Lib_Utils.h"              // SubclassCache, safe parsers, GetLocalPlayer, etc.
// Lib_ObjectFactory intentionally NOT included for RC: it carries UE4.27-specific
// hardcoding (FSD RVA 0x1F9C870, "FSD-Win64-Shipping.exe" module name, FFrame layout
// at 0x10/0x18/0x20/0x28) that does not apply to RogueCore. Needs a UE5.6 port
// (verify FStaticConstructObjectParameters and FFrame layouts against the game,
// and replace the RVA + call-site scan with RC-specific values) before re-enabling.
#include "../SharedLib/Lib_Print.h"              // PrintFieldValue, DumpItemProperties, Detail
#include "../SharedLib/Lib_EasyHook.h"           // MinHook wrapper
#include "../SharedLib/Lib_VTableHook.h"         // VTableHook class
#include "../SharedLib/Lib_GameHooks.h"          // ProcessEventHook, GameHooks API
#include "../SharedLib/Lib_VarSystem.h"          // VarSystem (commands deferred to Lib_CommandHandler.h)
#include "../SharedLib/Lib_CommandHandler.h"     // CommandContext, MutableContext, CommandHandler, VarSystem commands
#include "../SharedLib/Lib_PropertyInspector.h"  // PropertyInspector namespace
//#include "Lib_Json.h"             // JsonHook::Setup, JsonImpl::Parser, JSONType constants
#include "../SharedLib/Lib_KeyBindings.h"        // Key, Mod, Trigger, Focus, KeyBindings:: API
#include "../SharedLib/Lib_Math.h"               // FVector/FRotator/FLinearColor wrappers, Math:: utilities
