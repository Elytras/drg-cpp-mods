# ElytrasMods — Unreal Engine modding framework

A C++ DLL-injection modding framework for Unreal Engine games. Currently supports:

- **Deep Rock Galactic** (UE 4.27) — `DrgMods.dll`
- **Rogue Core** (UE 5.6, in progress) — `RcMods.dll`

A single CLI binary (`DrgCli.exe`) acts as both the injector and the REPL — it owns
the shared-memory channels, watches for the target game process, auto-injects the
matching DLL, and forwards commands to the running mod.

---

## Repository contents

Tracked directories at the repo root:

| Path | Role |
|---|---|
| `DrgCli/` | CLI + injector EXE (one binary, both jobs) |
| `DrgMods/` | DRG payload DLL — game-specific entry points, mods, generated SDK |
| `RcMods/` | RC payload DLL — same shape as `DrgMods/`, work in progress |
| `SharedLib/` | Game-neutral library code shared by both payloads (Utility project) |
| `ElytrasMods.sln` | Visual Studio 2022 solution |

Gitignored:

- `DrgMods/SDK/` and `RcMods/SDK/` — must be generated, see below.
- `DrgMods/ModOutput/` and `RcMods/ModOutput/` — runtime output.
- All build artifacts (`x64/`, `*.pdb`, `*.obj`, `.vs/`, etc.).

---

## SDK requirement

Game class definitions are not bundled — they must be generated from a live game
process using the fork of Dumper-7:

**<https://github.com/Elytras/Dumper-7>**

### Generating the SDK

1. Build the Dumper-7 fork in **x64 Release** → produces `Dumper-7.dll`.
2. Inject `Dumper-7.dll` into the running game. There are two ways:
   - Use any DLL injector of your choice, **or**
   - Place the DLL at `D:\Repos\Dumper7\x64\Release\Dumper-7.dll` (the path
     hardcoded in `DrgCli/CliTypes.h`, `kDumper7Path`) and run the `d7l` command
     from the CLI once the game is running.
3. Dumper-7 writes its output to `C:\Dumper-7\` (the default).
4. Copy the **contents of the `CppSDK/` subfolder** from the output into the
   matching SDK directory:
   - DRG → `DrgMods/SDK/`
   - RC  → `RcMods/SDK/`

If you want a different Dumper-7 location, edit `kDumper7Path` in
`DrgCli/CliTypes.h` (line 42).

---

## Dependencies

Managed via **vcpkg** (integrated with the solution):

| Package | Used for |
|---|---|
| `spdlog` | Logging throughout the framework |
| `minhook` | Low-level function hooking |

---

## Build

Open `ElytrasMods.sln` in **Visual Studio 2022** and build **x64 Release**
(or x64 Debug to attach the debugger to the game).

Project roles in the solution:

| Project | Output | Notes |
|---|---|---|
| `DrgMods` | `DrgMods.dll` | DRG payload. Builds against `DrgMods/SDK/`. |
| `RcMods` | `RcMods.dll` | RC payload. Builds against `RcMods/SDK/`. WIP. |
| `DrgCli` | `DrgCli.exe` | CLI + injector. Depends on neither DLL at link time. |
| `SharedLib` | none (Utility) | Lists shared `.h/.cpp` for IDE navigation only. |

`SharedLib` produces no output; each consumer project (`DrgMods`, `RcMods`)
ClCompiles the shared `.cpp` files via path references so they're compiled
against that project's own SDK headers.

---

## Workflow

```
DrgCli.exe         (auto-detect: DRG if neither process is running)
DrgCli.exe drg     (target FSD-Win64-Shipping.exe + DrgMods.dll)
DrgCli.exe rc      (target RogueCore-Win64-Shipping.exe + RcMods.dll)
```

On startup `DrgCli`:

1. Creates the shared-memory channels and named events for the selected profile.
2. Starts a process-watcher thread; when the target game process appears, it
   auto-injects the matching DLL.
3. Starts a hot-reload watcher — if the source DLL changes on disk, the running
   instance is unloaded and the fresh build re-injected.
4. Runs a split-pane REPL with tab completion, ghost hints, an autocomplete
   pane, and command history.

### Built-in CLI commands

| Command | Action |
|---|---|
| `load` | Inject the DLL into the running game process |
| `unload` | Unload the DLL; suppresses auto-inject until the next `load` |
| `reload` | Hot-reload the DLL (unload → re-inject) |
| `d7l` | Inject Dumper-7 from `kDumper7Path` |
| `d7u` | Unload Dumper-7 (disabled externally — use F6 in-game) |
| `mode [drg\|rc]` | Switch profile at runtime, with full shared-memory teardown/rebuild |
| `killgame` | `TerminateProcess` the target |
| `exit` | Quit the CLI |

Any other input is forwarded to the running DLL via the command channel. The DLL
registers its own commands (e.g. `set`, `get`, `unset`, plus whatever each loaded
mod adds); the CLI auto-pulls the registered command list whenever the DLL
signals `DRG_DllReady`, so tab-completion is always current.

---

## Architecture

### IPC layout — `SharedLib/IpcProtocol.h`

The shared-memory layouts and capacity constants live in `IpcProtocol.h`. The
per-game string constants (channel names, target process name, DLL filename)
live in each payload's own `Common.h`. For DRG (`DrgMods/Common.h`):

| Channel (named object) | Direction | Purpose |
|---|---|---|
| `Local\DRG_Logs` + `DRG_LogReady` | DLL → CLI | Log ring buffer (64 KB) |
| `Local\DRG_Commands` + `DRG_CmdReady` | CLI → DLL | Command buffer (4 KB) |
| `Local\DRG_Response` + `DRG_ResponseReady` | DLL → CLI | Tagged response (512 KB) |
| `Local\DRG_Shutdown` + `DRG_ShutdownDone` | — | Shutdown handshake |
| `Local\DRG_DllReady` | DLL → CLI | Triggers CLI auto-`listcmds` after each load |
| `Local\DRG_Meta` | CLI → DLL | 64-byte meta buffer carrying the CLI's `HWND` |

`ResponseBuffer` carries a tagged union (`ResponseType`):

- `Text` — plain string.
- `Scan` — `ScanResponse` listing scanned UE functions.
- `Commands` — `CommandsResponse` listing the DLL's registered commands.

The RC build mirrors the same layout with `RC_*` names.

### DLL lifecycle — `DrgMods/Main.cpp` (mirror in `RcMods/Main.cpp`)

`DllMain` (DLL_PROCESS_ATTACH) spawns a single `WorkerThread`, which:

1. Opens all shared-memory handles (`InitSharedMemory`).
2. Installs an `spdlog` sink that writes into the shared log ring buffer.
3. Reads the CLI's `HWND` from the meta buffer and initializes
   `KeyBindings` (focus-aware keybinding dispatch).
4. Constructs `ModManager` and calls `LoadMods()`.
5. Loops on `WaitForMultipleObjects`, handling shutdown / incoming commands /
   periodic `Update()` ticks (~100 ms cadence).

On unload: drains the mod manager, tears down keybindings, signals
`ShutdownDone`, and unmaps shared memory.

### SharedLib — game-neutral layer

Compiled separately into each consumer DLL (per-project `ClCompile` of the
SharedLib `.cpp` files; SharedLib itself produces nothing).

| Header | Role |
|---|---|
| `IpcProtocol.h` | Shared-memory layouts and capacity constants |
| `StringLib.h` | String utilities (`ToWide`, case-insensitive compare, …) |
| `UnrealCoreTypes.h` | Hand-written UE core type fallbacks |
| `Lib_EasyHook.h` | MinHook wrapper |
| `Lib_VTableHook.h` | VTable slot patching |
| `Lib_GameHooks.h` | `ProcessEventHook` singleton; lock-free callback dispatch |
| `Lib_ObjectCast.h` | `ObjectCast::Cast<T>`, `IsValidRaw`, `GObjectsOf<T>()` range |
| `Lib_ObjectFactory.h` | `NewObject<T>` via `StaticConstructObject_Internal` |
| `Lib_PropertyAccess.h` | `GetPropertyPtr`, `ReadBool`/`WriteBool`, `GetTypeName` |
| `Lib_PropertyInspector.h` | Property search, read/write, map/array element access |
| `Lib_Print.h` | `PrintFieldValue`, `DumpItemProperties`, tree-style output |
| `Lib_Scan.h` | `Scan::BuildFuncSig`, `ScanAllClasses` |
| `Lib_VarSystem.h` | Session-scoped typed variable storage (`g_Vars`) |
| `Lib_CommandHandler.h` | `CommandContext`, `CommandHandler`, built-in `set`/`get`/`unset` |
| `Lib_Json.h` | High-performance JSON parser hook (replaces UE Blueprint `UJSON_C`) |
| `Lib_KeyBindings.h` | Key binding registration and dispatch |
| `Lib_Math.h` | Alias: `namespace Math = SDK::Math` — math utilities live in `Basic.hpp` |

### Per-game layer (`DrgMods/`, `RcMods/`)

| File | Role |
|---|---|
| `Library.h` | Single include for all mod code; pulls SDK + every `Lib_*.h` in dependency order |
| `Common.h` | Process name, DLL filename, shared-memory / event name constants |
| `Lib_Forward.h` | Type aliases, `Game`/`Utils` namespace aliases, spdlog log fn shorthands |
| `Lib_Utils.h/.cpp` | `SubclassCache`, safe parsers, `GetLocalPlayer`, etc. |
| `Lib_VTableInfo.h` | VTable indices for UE virtual functions |
| `GameOffsets.h` | Per-game RVAs and call-site signatures (e.g. `StaticConstructObject_Internal`) |
| `Main.cpp` | `DllMain` + `WorkerThread` + shared-memory wiring |
| `ModManager.h/.cpp` | Mod loading, the game-thread tick, command dispatch |
| `Commands.h/.cpp` | Command registrations |
| `Aim.h/.cpp` *(DRG only)* | Aimbot, RCS, and silent-aim module |

### SDK (`DrgMods/SDK/`, `RcMods/SDK/`)

Generated by Dumper-7 (gitignored). Files consumed by the framework:

| File | Purpose |
|---|---|
| `SDK/SDK/Basic.hpp` | Core UE types: `UObject`, `FName`, `FField`, `TArray`, `FString`; plus `SDK::Math`, `SDK::FieldCast`, `UnrealAllocator`, `UE_New`/`UE_Delete`, and per-build offsets |
| `SDK/SDK/Engine_classes.hpp` | Engine class definitions |
| `SDK/SDK/FSD_classes.hpp` *(DRG)* / `SDK/SDK/RogueCore_classes.hpp` *(RC)* | Game class definitions |
| `SDK/SDK/CoreUObject_classes.hpp` | CoreUObject definitions |
| `SDK/UnrealContainers.hpp` | `TArray`, `TMap`, `FString`, `TSet` implementations |

---

## License

MIT — Copyright (c) 2026 Dmytro Shestopal. See [LICENSE.txt](LICENSE.txt).

### Third-party

- **spdlog** — MIT ([github.com/gabime/spdlog](https://github.com/gabime/spdlog))
- **MinHook** — BSD 2-Clause ([github.com/TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook))
- **Dumper-7 (fork)** — [github.com/Elytras/Dumper-7](https://github.com/Elytras/Dumper-7)
