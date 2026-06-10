# CAPABILITIES — what's already provided, and what to use

**Read this before writing any helper.** Almost every "small utility" you might write
already exists. The recurring tech-debt in this repo is *re-implementing* things
(case-insensitive compare, GObjects walks, number parsing, name access) because the
canonical version wasn't discoverable. This file is the index that makes it
discoverable. If you add a broadly-useful helper, add it to the right layer **and add
a row here**.

---

## "I need to… → use this" (canonical helpers)

| Need | Use | Header | Don't |
|---|---|---|---|
| Case-insensitive compare (equals / substring / prefix) | `StringLib::IEquals` / `IContains` / `IStartsWith` | `StringLib.h` | `tolower` loop, `ContainsCI`, `IEqualsStr`, `std::search`+tolower |
| Split / join / trim / replace / to-lower/upper | `StringLib::SplitString` / `JoinStringArray` / `Trim` / `Replace` / `ToLower` / `ToUpper` | `StringLib.h` | manual loops |
| UTF-8 ↔ wide | `StringLib::ToWide` / `ToNarrow` | `StringLib.h` | raw `MultiByteToWideChar` |
| Parse number from string (no throw) | `SafeStof` / `SafeStod` / `SafeStoll` / `SafeStoull` | `CoreUtils.h` | `std::stof`/`std::strtof`/`atoi` |
| Membership test (variadic) | `AnyOf(x, a, b, c)` / `NoneOf(x, a, b, c)` | `CoreUtils.h` | chained `==`/`!=` |
| Steady time / sleep (ms) | `GetTimeMs()` / `SleepNow(ms)` | `CoreUtils.h` | raw `chrono`/`this_thread` |
| Float compare (relative eps) | `NearlyEqual(a, b)` | `CoreUtils.h` | hand-rolled `fabs` compare |
| Indexable container fallback | `GetOrDefault(c, def, idx?)` | `CoreUtils.h` | manual `empty()`/bounds check |
| Integer/float range loop | `range(n)` / `range(b, e[, step])` / `numeric_range<T>` | `CoreUtils.h` | manual index `for` loop |
| Iterate all UObjects / actors | `for (auto* o : GObjectsOf<SDK::AActor>())` | `Lib_ObjectCast.h` | `for i in GObjects->Num()` + manual IsValid/CDO/IsA |
| Cast a UObject (checked) | `ObjectCast::Cast<T>(obj)` (null if wrong type) | `Lib_ObjectCast.h` | raw `static_cast` after a hand IsA |
| Validity check | `IsValid(obj)` (engine) / `IsValidRaw(obj)` (flags) / `IsValidOf<T>(obj)` (validity+type) | `Lib_Forward.h` / `Lib_ObjectCast.h` / `Lib_Utils.h` | bespoke flag tests |
| Object display name | `GetDisplayName(obj)` | `Lib_Utils.h` | raw `obj->GetName()` for UI |
| Is-subclass-of (cached) | `SubclassCache::Get().IsSubclassOf(d, b)` or `IsChildOf<T>(cls)` | `Lib_Utils.h` | walking SuperStruct by hand |
| Walk class ancestry | `UClassHierarchyRange(cls)` | `Lib_ObjectCast.h` | manual `SuperStruct` loop |
| Get typed outer | `GetTypedOuter<T>(obj)` | `Lib_Utils.h` | manual `Outer` loop |
| Local player / controller / state | `GetLocalPlayer()` / `GetLocalController()` / `GetLocalPlayerState()` | `Lib_Utils.h` | re-deriving from world |
| Current world | `GetWorld()` | `Lib_Forward.h` | caching a stale UWorld* |
| Log | `info/warn/error/debug/trace(fmt, …)` (spdlog fmt) | `Lib_Forward.h` | `printf`, `std::cout` |
| Run work on the game thread (once) | `EnqueueOnce(fn)` | `Lib_GameHooks.h` | touching SDK off the game thread |
| Run on game thread every N ticks | `EnqueueEveryNTicks(n, fn)` | `Lib_GameHooks.h` | a private polling thread |
| Share a game-thread snapshot with the UI thread | `GameThreadSnapshot<T>` (read/with/store/request/setAuto/beat/due) | `GameThreadSnapshot.h` | hand-rolled mutex + snapshot + request/auto/heartbeat atomics |
| Hook ProcessEvent / EngineTick / SCO | `GameHooks::OnProcessEvent()…` / `OnEngineTick` / `OnStaticConstructObject` | `Lib_GameHooks.h` | raw MinHook |
| Toggle-style hook command | `HookToggle<Hook>` | `Lib_GameHooks.h` | hand-managed `CallbackHandle` |
| Install/remove a MinHook | `Lib_EasyHook` (ref-counted) | `Lib_EasyHook.h` | direct `MH_*` |
| Read/write a property by offset | `GetPropertyPtr/Ref/Value`, `ReadBool/WriteBool` | `Lib_PropertyAccess.h` | raw `reinterpret_cast` math |
| Property type name | `GetTypeName(field)` | `Lib_PropertyAccess.h` | — |
| Session variable (typed) | `VarSystem` (`g_Vars`, `set/get/unset`) | `Lib_VarSystem.h` | ad-hoc globals |
| Register a command | `handler.Register(name, fn, category, desc)` | `Lib_CommandHandler.h` | — |
| Dispatch a command (game-thread safe from UI) | `EnqueueOnce([]{ handler.Dispatch(line, 0); })` | `Lib_CommandHandler.h` | calling SDK directly from the overlay thread |
| Send a response to the CLI | `SendResponse(seq, msg)`; mirror via `g_responseTap` | `Lib_CommandHandler.h` | — |
| Keybinding | `KeyBindings::Register(...)` / CLI `bind`; parse via `ParseChord` | `Lib_KeyBindings.h` | raw LL hooks |
| Read config.yaml | `NetLogConfig::Load()` / `ConfigPath()` | `Lib_NetLogConfig.h` | bespoke YAML loaders |

---

## Layer model — a file's layer is its folder

**A layer may only depend on layers above it.** Each project puts all four
`SharedLib\{core,hooks,overlay,game}` folders on its include path, so a bare
`#include "Foo.h"` resolves regardless of which folder `Foo` lives in.

| Layer | Where | May use | Provides |
|---|---|---|---|
| **core** (game-agnostic, **no `SDK::`**) | `SharedLib/core/` — `StringLib.h`, `IpcProtocol.h`, `CoreUtils.h` (`SafeSto*`, `NoneOf/AnyOf`, `GetTimeMs/SleepNow`, `NearlyEqual`, `GetOrDefault`, `range`/`numeric_range`), `GameThreadSnapshot.h` | nothing internal (STL only) | compiles without any SDK; safe for CLI + both DLLs |
| **hooks** (uses `SDK::`, per-game) | `SharedLib/hooks/` — `Lib_EasyHook`, `Lib_VTableHook`, `Lib_GameHooks` | core + per-game foundation | MinHook wrapper, ProcessEvent/EngineTick/SCO hooks, `Enqueue*` |
| **game** (uses `SDK::`, per-game) | `SharedLib/game/` — `Lib_ObjectCast`, `Lib_PropertyAccess`, `Lib_FField`, `Lib_Scan`, `Lib_Print`, `Lib_Json`, `Lib_ObjectFactory`, `Lib_PropertyInspector`, `Lib_VarSystem`, `Lib_CommandHandler`, `Lib_KeyBindings`, `Lib_NetLogConfig`, `Lib_ActorList`, `ModManager`, `SharedCommands`, `UnrealCoreTypes.h` | core + hooks + per-game foundation | the SDK-aware helper library |
| **overlay** (uses `SDK::`, per-game) | `SharedLib/overlay/` — `Lib_Overlay`, `Lib_OverlayConsole` | core + hooks + game | the in-game ImGui UI subsystem |
| **per-game foundation** | `DrgMods/` & `RcMods/` — `Lib_Forward.h`, `Common.h`, generated `SDK/`, per-game `Lib_Utils.h`, `Library.h` umbrella | — | `info/warn/error`, `IsValid`, `GetWorld`, `Kismet`/`MathLib`, the `SDK` namespace. **Only layer that names a game's types freely.** |
| **per-game host** | `DrgMods/` & `RcMods/` — `Commands.cpp`, `Aimbot_*`, `BpModLoader`, `Main.cpp` | everything | DLL entry, command registration, per-game policy hooks |

Note: the per-game `Lib_Utils.h` holds only game-specific helpers now (`GetItem`,
player accessors, `GetActorOfClass`, weapon getters) and `#include`s `core/CoreUtils.h` —
the old duplicate-parsers leak that drove SharedLib reinvention is fixed.

---

## Anti-patterns / review checklist (confirm before merging)

- ❌ A new `tolower`/`towlower` loop or local `IEquals`/`ContainsCI`. → `StringLib::I*`.
- ❌ `for (i = 0; i < …GObjects->Num(); …)`. → `GObjectsOf<T>()`.
- ❌ `std::stof` / `std::stoi` / `std::strtof` / `atoi` in DLL code. → `SafeSto*`.
- ❌ Raw `obj->GetName()` for anything user-facing. → `GetDisplayName(obj)`.
- ❌ `static_cast<T*>(obj)` without a verified `IsA`. → `ObjectCast::Cast<T>`.
- ❌ Touching `SDK::`/world state off the game thread. → `EnqueueOnce` / a snapshot.
- ❌ A private polling `std::thread` for game state. → `EnqueueEveryNTicks`.
- ❌ A new anon-namespace helper that's broadly useful. → promote it to the right layer
  and add a row above, so the next person finds it.
- ❌ Caching a `UWorld*`/`UObject*` across a world change without revalidating.
- ☑ A new SharedLib `.cpp` is added to **both** `DrgMods.vcxproj` and `RcMods.vcxproj`
  (with the `SharedLib\<layer>\` path), and lands in the correct layer folder.
- ☑ Builds **DRG + RC** (+ **CLI** if touched) clean before merging.

---

## Maintenance

When you add or move a capability, update **this table**, the relevant layer note, and
(if it changes the canonical answer) the `reference` memory `reference_capability_map`.
This file is the source of truth the memory points at.
