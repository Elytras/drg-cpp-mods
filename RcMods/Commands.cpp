#include "Commands.h"
#include "Library.h"
#include <cmath>

using namespace SDK;
using namespace VarSystem;

extern HANDLE g_hRespEvent;

// =========================================================================
// Tick system — stub for RogueCore
// =========================================================================

namespace TickSystem
{
    void Dispatch(long double /*actualDeltaMs*/) {}
    void Reset()                                 {}
}

// =========================================================================
// Per-toggle callback handles
// =========================================================================

namespace State
{
    GameHooks::CallbackHandle TickCallback       = 0;
    GameHooks::CallbackHandle BeginPlayCallback  = 0;
    GameHooks::CallbackHandle LogAllEventsHandle = 0;

    // ── Call-command scan cache ───────────────────────────────────────────
    struct ScannedFunction
    {
        SDK::UFunction* Func          = nullptr;
        SDK::UObject*   Owner         = nullptr;
        std::string     FunctionName;
        std::string     OwnerName;
        std::string     OwnerClassName;
        std::string     ExplicitName;
    };
    std::unordered_map<std::string, ScannedFunction>          ScannedFunctions;
    std::unordered_map<std::string, std::vector<std::string>> ScannedFunctionVariantsByName;
}

void ResetCallbackHandles()
{
    State::TickCallback       = 0;
    State::BeginPlayCallback  = 0;
    State::LogAllEventsHandle = 0;
}

// =========================================================================
// Default callbacks
// =========================================================================

void InitDefaultCallbacks()
{
    VarSystem::RegisterBuiltinBindings();
}

// =========================================================================
// Scan — populates the call-command cache
// =========================================================================

namespace Scan
{
    void DoScan()
    {
        APlayerController* LocalCtrl = GetLocalController();
        if (!IsValidOf<APlayerController>(LocalCtrl)) { warn("[scan] No local controller."); return; }
        APawn*        LocalPawn  = LocalCtrl->K2_GetPawn();
        APlayerState* LocalState = LocalCtrl->PlayerState;

        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass(), &AllActors);
        State::ScannedFunctions.clear();
        State::ScannedFunctionVariantsByName.clear();
        int inserted = 0;

        auto TryScanObject = [&](UObject* Obj)
        {
            for (UClass* cls : UClassHierarchyRange(Obj->Class))
                for (auto* field : UFieldRange(cls->Children))
                {
                    if (!field->IsA(UFunction::StaticClass())) continue;
                    auto* Func  = static_cast<UFunction*>(field);
                    auto  flags = static_cast<EFunctionFlags>(Func->FunctionFlags);
                    if (!(flags & EFunctionFlags::Net))       continue;
                    if (!(flags & EFunctionFlags::NetServer)) continue;
                    std::string name        = Func->GetName();
                    std::string explicitName = BuildExplicitCallName(Obj, Func);
                    State::ScannedFunctions[explicitName] = {
                        Func, Obj, name,
                        Obj ? Obj->GetName() : "?",
                        Obj && Obj->Class ? Obj->Class->GetName() : "?",
                        explicitName
                    };
                    State::ScannedFunctionVariantsByName[name].push_back(explicitName);
                    ++inserted;
                }
        };

        for (auto* Actor : AllActors)
        {
            if (!Actor || !Kismet::IsValid(Actor) || !Actor->bReplicates) continue;
            if (NoneOf<AActor*>(Actor->GetOwner(), LocalCtrl, LocalPawn, LocalState)) continue;
            TryScanObject(Actor);
            TArray<UActorComponent*> Components =
                Actor->K2_GetComponentsByClass(UActorComponent::StaticClass());
            for (auto* Comp : Components)
                if (Comp && Kismet::IsValid(Comp)) TryScanObject(Comp);
        }

        for (auto& [name, variants] : State::ScannedFunctionVariantsByName)
        {
            std::sort(variants.begin(), variants.end());
            variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
        }
        info("[scan] Scan complete — {} server RPCs.", inserted);
    }
}

// =========================================================================
// Generic commands  (game-agnostic — work against any UE SDK)
// =========================================================================

namespace RcCmd
{
    static void FindClass(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 2) { warn("[cmd:findclass] usage: findclass <name>"); return; }
        const std::string& needle = ctx.Arg(1);
        int hits = 0;
        for (int i = 0; i < UObject::GObjects->Num(); ++i)
        {
            auto* obj = UObject::GObjects->GetByIndex(i);
            if (!obj || !obj->IsA(UClass::StaticClass())) continue;
            std::string n = obj->GetName();
            if (PropertyInspector::NameMatches(n, needle, true))
            {
                auto* cls = static_cast<UClass*>(obj);
                info("  [class] {} -> CDO: {}", n,
                    cls->ClassDefaultObject ? cls->ClassDefaultObject->GetName() : "null");
                ++hits;
            }
        }
        info("[cmd:findclass] {} match(es) for '{}'", hits, needle);
    }

    static void ScanAll(const CommandContext&)
    {
        Scan::ScanAllClasses();
    }

    // Scan replicated actors for server RPCs — populates the `call` cache
    // AND sends the ScanResponse for CLI autocomplete.
    static void ScanFuncs(const CommandContext& ctx)
    {
        Scan::DoScan();

        ScanResponse sr{};
        auto WriteToSr = [&](const State::ScannedFunction& sf)
        {
            if (sr.count >= MAX_SCAN_RESULT) return;
            auto& out = sr.funcs[sr.count++];

            // Parse params out of the pretty-printed signature.
            std::string sig    = Scan::BuildFuncSig(sf.Func);
            auto        paren  = sig.find('(');
            std::string params;
            if (paren != std::string::npos)
            {
                params = sig.substr(paren + 1);
                auto close = params.rfind(')');
                if (close != std::string::npos) params = params.substr(0, close);
                for (auto& c : params) if (c == '\n' || c == '\t' || c == '|') c = ' ';
                auto ne = std::unique(params.begin(), params.end(),
                    [](char a, char b) { return a == ' ' && b == ' '; });
                params.erase(ne, params.end());
                if (!params.empty() && params.front() == ' ') params = params.substr(1);
                if (!params.empty() && params.back()  == ' ') params.pop_back();
            }

            strncpy_s(out.name,   sf.FunctionName.c_str(),   MAX_FUNC_NAME - 1);
            strncpy_s(out.owner,  sf.OwnerClassName.c_str(), MAX_FUNC_NAME - 1);
            strncpy_s(out.params, params.c_str(),             MAX_PARAM_STR - 1);
        };

        for (auto& [key, sf] : State::ScannedFunctions)
            WriteToSr(sf);

        info("[scanfuncs] Done — {} server RPCs (populates `call` autocomplete)", sr.count);

        if (g_pRespBuffer && !g_pRespBuffer->ready.load(std::memory_order_acquire))
        {
            g_pRespBuffer->type = ResponseType::Scan;
            memcpy(&g_pRespBuffer->data.scan, &sr, sizeof(sr));
            g_pRespBuffer->seq.store(ctx.seq, std::memory_order_release);
            g_pRespBuffer->ready.store(true, std::memory_order_release);
            SetEvent(g_hRespEvent);
        }
    }

    static void Exec(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 2) { warn("[cmd:exec] usage: exec <command>"); return; }
        std::string cmd;
        for (size_t i = 1; i < ctx.args.size(); ++i) { if (i > 1) cmd += ' '; cmd += ctx.args[i]; }
        ::Exec(cmd);
    }

    static void LogAllEvents(const CommandContext&)
    {
        if (State::LogAllEventsHandle == 0)
        {
            State::LogAllEventsHandle = GameHooks::OnProcessEventAll(
                [](UObject* Object, UFunction* Function, void*)
                {
                    if (!Object || !Function) return;
                    info("[PE] {}::{}",
                        Object->Class ? Object->Class->GetName() : "?",
                        Function->GetName());
                });
            info("[cmd:logallevents] enabled (every ProcessEvent will be logged)");
        }
        else
        {
            GameHooks::RemoveHook(State::LogAllEventsHandle);
            State::LogAllEventsHandle = 0;
            info("[cmd:logallevents] disabled");
        }
    }

    static void StopTick(const CommandContext&)
    {
        if (State::TickCallback == 0)
        {
            State::TickCallback = GameHooks::OnProcessEventByNameAndClass(
                "ReceiveTick", AActor::StaticClass(),
                [](UObject* Object, UFunction*, void*)
                {
                    if (Object) info("[ReceiveTick] {}", Object->GetName());
                });
            info("[cmd:stoptick] tick listener enabled");
        }
        else
        {
            GameHooks::RemoveHook(State::TickCallback);
            State::TickCallback = 0;
            info("[cmd:stoptick] tick listener disabled");
        }
    }

    static void BeginPlay(const CommandContext&)
    {
        if (State::BeginPlayCallback == 0)
        {
            State::BeginPlayCallback = GameHooks::OnProcessEventByNameAndClass(
                "ReceiveBeginPlay", AActor::StaticClass(),
                [](UObject* Object, UFunction*, void*)
                {
                    if (!Object || !Object->Class) return;
                    info("[BeginPlay] {} spawned: {}",
                        Object->Class->Name.ToString(), Object->Name.ToString());
                });
            info("[cmd:beginplay] watcher enabled");
        }
        else
        {
            GameHooks::RemoveHook(State::BeginPlayCallback);
            State::BeginPlayCallback = 0;
            info("[cmd:beginplay] watcher disabled");
        }
    }

    // ── call ─────────────────────────────────────────────────────────────────
    // Invoke a server RPC by name.  Use `scanfuncs` first to populate the
    // cache; call will auto-rescan on a cache miss.
    //
    // Syntax:  call <FuncName> [arg0 arg1 ...]
    //          call <Owner>::<FuncName> [arg0 ...]   (disambiguate duplicates)

    static void Call(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 2) { warn("[cmd:call] Usage: call <FunctionName> [arg0 arg1 ...]"); return; }

        std::vector<std::string> rest{ ctx.args.begin() + 1, ctx.args.end() };
        std::string funcName;
        std::vector<std::string> rawArgs;

        auto sep = std::find(rest.begin(), rest.end(), "::");
        if (sep == rest.end())
        {
            funcName = rest[0];
            rawArgs  = { rest.begin() + 1, rest.end() };
        }
        else
        {
            for (auto it = rest.begin(); it != sep; ++it) { if (!funcName.empty()) funcName += ' '; funcName += *it; }
            rawArgs = { sep + 1, rest.end() };
        }

        if (!State::ScannedFunctions.contains(funcName) &&
            !State::ScannedFunctionVariantsByName.contains(funcName))
        {
            info("[cmd:call] '{}' not cached, rescanning...", funcName);
            Scan::DoScan();
        }

        auto PrintVariants = [&](const std::string& baseName)
        {
            auto vit = State::ScannedFunctionVariantsByName.find(baseName);
            if (vit == State::ScannedFunctionVariantsByName.end() || vit->second.empty()) return;
            info("[cmd:call] '{}' is ambiguous; use one of:", baseName);
            for (size_t i = 0; i < vit->second.size(); i++)
                info("[cmd:call]   [{}] {}", i, vit->second[i]);
        };

        auto ResolveCachedCallTarget = [&]() -> State::ScannedFunction*
        {
            auto ResolveUniqueVariant = [&](const std::string& key) -> State::ScannedFunction*
            {
                auto it = State::ScannedFunctions.find(key);
                return it == State::ScannedFunctions.end() ? nullptr : &it->second;
            };

            auto ResolveByBaseName = [&](const std::string& baseName) -> State::ScannedFunction*
            {
                auto vit = State::ScannedFunctionVariantsByName.find(baseName);
                if (vit == State::ScannedFunctionVariantsByName.end() || vit->second.empty()) return nullptr;
                if (vit->second.size() != 1) { PrintVariants(baseName); return nullptr; }
                return ResolveUniqueVariant(vit->second[0]);
            };

            State::ScannedFunction* candidate = ResolveUniqueVariant(funcName);
            if (!candidate) candidate = ResolveByBaseName(funcName);
            if (!candidate) return nullptr;

            auto& [Func, Owner, FunctionName, OwnerName, OwnerClassName, ExplicitName] = *candidate;
            if (!Owner || !IsValidRaw(Owner) || !IsValid(Owner) || !IsInActiveWorld(Owner))
            {
                const std::string explicitName = ExplicitName;
                const std::string functionName = FunctionName;
                warn("[cmd:call] Owner stale or from old world for '{}'; rescanning.", funcName);
                Scan::DoScan();
                candidate = ResolveUniqueVariant(explicitName);
                if (!candidate) candidate = ResolveByBaseName(functionName);
                if (!candidate) return nullptr;
            }

            if (!IsValidOf<SDK::UFunction>(candidate->Func))
            {
                const std::string explicitName = candidate->ExplicitName;
                const std::string functionName = candidate->FunctionName;
                warn("[cmd:call] UFunction stale for '{}'; rescanning.", funcName);
                Scan::DoScan();
                candidate = ResolveUniqueVariant(explicitName);
                if (!candidate) candidate = ResolveByBaseName(functionName);
                if (!candidate) return nullptr;
            }

            return candidate;
        };

        State::ScannedFunction* target = ResolveCachedCallTarget();
        if (!target) { warn("[cmd:call] '{}' not found.", funcName); return; }

        auto* Func  = target->Func;
        auto* Owner = target->Owner;

        std::vector<SDK::FProperty*> parmProps;
        for (SDK::FField* field : SDK::FFieldRange(Func->ChildProperties))
        {
            if (!SDK::FieldCast::IsA<SDK::FProperty>(field)) continue;
            SDK::FProperty*     Prop = static_cast<SDK::FProperty*>(field);
            SDK::EPropertyFlags pf   = static_cast<SDK::EPropertyFlags>(Prop->PropertyFlags);
            if (!(pf & SDK::EPropertyFlags::Parm))    continue;
            if (pf & SDK::EPropertyFlags::ReturnParm) continue;
            parmProps.push_back(Prop);
        }

        const int32 parmsSize = PropertyInspector::ComputeParmsSize(Func);
        std::vector<uint8> parmsBuf(parmsSize, 0);

        if (rawArgs.size() > parmProps.size())
            warn("[cmd:call] {} args given but '{}' only has {} param(s); extras ignored.",
                rawArgs.size(), funcName, parmProps.size());

        size_t filled = std::min(rawArgs.size(), parmProps.size());
        for (size_t i = 0; i < filled; ++i)
        {
            ExpandResult expanded = VarSystem::Expand(rawArgs[i]);
            if (!expanded.isValid) continue;
            uintptr_t base = reinterpret_cast<uintptr_t>(parmsBuf.data());
            if (expanded.object)
            {
                SDK::FieldCast::Visit(parmProps[i], [&]<typename T>(T* p)
                {
                    if constexpr (std::is_same_v<T, SDK::FObjectProperty>     ||
                                  std::is_same_v<T, SDK::FObjectPropertyBase> ||
                                  std::is_same_v<T, SDK::FClassProperty>      ||
                                  std::is_same_v<T, SDK::FWeakObjectProperty>)
                        *GetPropertyPtr<SDK::UObject*>(base, p->Offset) = expanded.object;
                    else
                    {
                        warn("[cmd:call]   [{}] param '{}' not object property — name fallback",
                            i, p->Name.ToString());
                        PropertyInspector::WriteParam(parmProps[i], expanded.object->GetName(), parmsBuf.data());
                    }
                });
            }
            else PropertyInspector::WriteParam(parmProps[i], expanded.token, parmsBuf.data());
        }

        if (!parmProps.empty())
        {
            info("[cmd:call] '{}' ({} param(s), {} supplied):", funcName, parmProps.size(), rawArgs.size());
            for (size_t i = 0; i < parmProps.size(); ++i)
            {
                std::string display;
                if (i >= rawArgs.size()) display = "<zeroed>";
                else
                {
                    ExpandResult expanded = VarSystem::Expand(rawArgs[i]);
                    if (rawArgs[i] == expanded.token) display = rawArgs[i];
                    else if (expanded.object)         display = rawArgs[i] + " -> [object] " + expanded.object->GetName();
                    else                              display = rawArgs[i] + " -> " + expanded.token;
                }
                info("[cmd:call]   [{}] {} = {}", i, parmProps[i]->Name.ToString(), display);
            }
        }

        uint32 savedFlags = Func->FunctionFlags;
        if (static_cast<SDK::EFunctionFlags>(Func->FunctionFlags) & SDK::EFunctionFlags::Native)
            Func->FunctionFlags |= 0x400;
        Owner->ProcessEvent(Func, parmsSize > 0 ? parmsBuf.data() : nullptr);
        Func->FunctionFlags = savedFlags;

        // Destruct any string/name params we constructed in the buffer.
        for (SDK::FProperty* Prop : parmProps)
            SDK::FieldCast::Visit(Prop, [&]<typename T>(T* p)
            {
                uintptr_t base = reinterpret_cast<uintptr_t>(parmsBuf.data());
                if constexpr (std::is_same_v<T, SDK::FStrProperty>)
                    GetPropertyPtr<FString>(base, p->Offset)->~FString();
                else if constexpr (std::is_same_v<T, SDK::FNameProperty>)
                    GetPropertyPtr<FName>(base, p->Offset)->~FName();
            });

        info("[cmd:call] Called '{}' on '{}'.", funcName, Owner->GetName());
    }

    // ── norecoil ─────────────────────────────────────────────────────────────
    // Toggle recoil compensation (RCS). Runs on UEngine::Tick (After) and
    // counteracts camera drift caused by server-applied recoil each frame.
    // No aimbot integration — pure camera stabilisation only.

    static void NoRecoil(const CommandContext&)
    {
        static bool  enabled      = false;
        static bool  initialized  = false;
        static float desiredPitch = 0.f, prevCtrlPitch = 0.f;
        static float desiredYaw   = 0.f, prevCtrlYaw   = 0.f;
        static GameHooks::CallbackHandle handle = 0;

        enabled     = !enabled;
        initialized = false;

        if (enabled)
        {
            auto normP = [](float a) -> float
            {
                a = std::fmod(a, 360.f);
                if (a >  180.f) a -= 360.f;
                if (a < -180.f) a += 360.f;
                return a;
            };

            handle = GameHooks::OnEngineTick(
                [normP](UEngine*, float, bool)
                {
                    if (!enabled) return;

                    APlayerController* Ctrl = GetLocalController();
                    if (!IsValidOf<APlayerController>(Ctrl)) return;

                    APlayerCameraManager* CamMgr = Ctrl->PlayerCameraManager;
                    if (!CamMgr) return;

                    const FRotator ctrlRot   = Ctrl->GetControlRotation();
                    const FRotator camRot    = CamMgr->CameraCachePrivate.POV.Rotation;
                    const float    ctrlPitch = normP(ctrlRot.Pitch);
                    const float    camPitch  = normP(camRot.Pitch);
                    const float    ctrlYaw   = normP(ctrlRot.Yaw);
                    const float    camYaw    = normP(camRot.Yaw);

                    if (!initialized)
                    {
                        desiredPitch  = ctrlPitch;
                        prevCtrlPitch = ctrlPitch;
                        desiredYaw    = ctrlYaw;
                        prevCtrlYaw   = ctrlYaw;
                        initialized   = true;
                        return;
                    }

                    const float ctrlPitchDelta = normP(ctrlPitch - prevCtrlPitch);
                    const float ctrlYawDelta   = normP(ctrlYaw   - prevCtrlYaw);
                    const float pitchOffset    = camPitch - ctrlPitch;
                    const float yawOffset      = normP(camYaw - ctrlYaw);

                    // Gimbal-flip protection: a single-frame delta > 90° is never
                    // real input or recoil — it's UE reflecting pitch at ±90°.
                    // Resync desired rotation and skip the frame.
                    constexpr float kGimbalThreshold = 90.f;
                    if (std::abs(yawOffset)    > kGimbalThreshold ||
                        std::abs(ctrlYawDelta) > kGimbalThreshold ||
                        std::abs(pitchOffset)  > kGimbalThreshold)
                    {
                        desiredPitch  = ctrlPitch;
                        desiredYaw    = ctrlYaw;
                        prevCtrlPitch = ctrlPitch;
                        prevCtrlYaw   = ctrlYaw;
                        return;
                    }

                    desiredPitch += ctrlPitchDelta;
                    desiredPitch  = std::clamp(desiredPitch, -90.f, 90.f);
                    desiredYaw   += ctrlYawDelta;
                    desiredYaw    = normP(desiredYaw);

                    // ctrl = desired - recoil_offset  →  camera = ctrl + offset = desired
                    const float newPitch = std::clamp(desiredPitch - pitchOffset, -90.f, 90.f);
                    const float newYaw   = normP(desiredYaw - yawOffset);

                    FRotator rot = ctrlRot;
                    rot.Pitch    = newPitch;
                    rot.Yaw      = newYaw;
                    Ctrl->SetControlRotation(rot);
                    prevCtrlPitch = newPitch;
                    prevCtrlYaw   = newYaw;
                },
                GameHooks::ExecutionTiming::After
            );
        }
        else if (handle != 0)
        {
            GameHooks::EngineTickHook::Get().RemoveCallback(handle);
            handle = 0;
        }

        info("[norecoil] {}", enabled ? "ON" : "OFF");
    }

} // namespace RcCmd

// =========================================================================
// Command registration
// =========================================================================

void RegisterCommands(CommandHandler& handler)
{
    using namespace VarSystem;

    // Inspection
    handler.Register("findclass", RcCmd::FindClass, "Inspection",
        R"(Find classes by name (fuzzy substring))");
    handler.Register("prop",      PropertyInspector::DispatchCommand, "Inspection",
        R"(prop <cdo|obj> <n> <dump|get|set|list> [prop] [value] [fuzzy] [class <name>])");
    handler.Register("scanall",   RcCmd::ScanAll,   "Inspection",
        R"(Scan all UClass CDOs for Net+NetServer server RPCs)");
    handler.Register("scanfuncs", RcCmd::ScanFuncs, "Inspection",
        R"(Scan replicated actors + components for server RPCs (populates `call` autocomplete))");

    // System
    handler.Register("exec",         RcCmd::Exec,         "System",
        R"(Execute a console command: exec <command>)");
    handler.Register("logallevents", RcCmd::LogAllEvents, "System",
        R"(Toggle logging of ALL ProcessEvent calls (very verbose))");
    handler.Register("stoptick",     RcCmd::StopTick,     "System",
        R"(Toggle ReceiveTick event logging on AActor)");
    handler.Register("beginplay",    RcCmd::BeginPlay,    "System",
        R"(Toggle BeginPlay spawn watcher on AActor)");
    handler.Register("call",         RcCmd::Call,         "System",
        R"(Call a server RPC: call <FunctionName> [arg0 arg1 ...])");

    // Player
    handler.Register("norecoil", RcCmd::NoRecoil, "Player",
        R"(Toggle recoil compensation (RCS) — stabilises camera against server-applied recoil)");

    // Variables
    handler.Register("get",   Cmd_Get,   "Variables", R"(Get a variable: get <n>)");
    handler.Register("set",   Cmd_Set,   "Variables", R"(Set a variable: set <n> <value>)");
    handler.Register("unset", Cmd_Unset, "Variables", R"(Delete a variable: unset <n>)");
    handler.Register("vars",  Cmd_Vars,  "Variables", R"(List all variables)");

    // Keybindings
    KeyBindings::RegisterCommands(handler);
}

// =========================================================================
// SendCommandList
// =========================================================================

void SendCommandList(const CommandContext& ctx, const CommandHandler& handler)
{
    auto& entries = handler.GetEntries();
    if (entries.empty()) return;

    if (!g_pRespBuffer || !g_hRespEvent) return;

    constexpr DWORD MAX_WAIT_MS = 1000;
    DWORD deadline = GetTickCount() + MAX_WAIT_MS;
    while (g_pRespBuffer->ready.load(std::memory_order_acquire))
    {
        if (GetTickCount() > deadline) return;
        Sleep(5);
    }

    auto& resp = g_pRespBuffer->data.commands;
    resp.count = 0;

    for (auto& [name, entry] : entries)
    {
        if (resp.count >= MAX_CMD_COUNT) break;
        auto& ce = resp.cmds[resp.count++];
        strncpy_s(ce.name, name.c_str(), MAX_CMD_NAME - 1);
        strncpy_s(ce.desc, entry.description.c_str(), MAX_CMD_DESC - 1);
    }

    g_pRespBuffer->type = ResponseType::Commands;
    g_pRespBuffer->seq.store(ctx.seq, std::memory_order_release);
    g_pRespBuffer->ready.store(true, std::memory_order_release);
    SetEvent(g_hRespEvent);
}
