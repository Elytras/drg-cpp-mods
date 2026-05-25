#include "Commands.h"
#include "Library.h"

using namespace SDK;

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
// Per-toggle callback handles (used by stoptick / beginplay / logallevents)
// =========================================================================

namespace State
{
    GameHooks::CallbackHandle TickCallback        = 0;
    GameHooks::CallbackHandle BeginPlayCallback   = 0;
    GameHooks::CallbackHandle LogAllEventsHandle  = 0;
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

    // Writes scan results both to the log (boxed tree) and to the IPC
    // ScanResponse so the CLI can populate `call` autocomplete.
    static void ScanFuncs(const CommandContext& ctx)
    {
        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass(), &AllActors);

        int totalActors = 0, totalFuncs = 0;
        ScanResponse sr{};

        auto WriteToSr = [&](const std::string& sig, const std::string& owner)
        {
            if (sr.count >= MAX_SCAN_RESULT) return;
            auto& out = sr.funcs[sr.count++];
            auto paren = sig.find('(');
            std::string fname = paren != std::string::npos ? sig.substr(0, paren) : sig;
            std::string params;
            if (paren != std::string::npos)
            {
                params = sig.substr(paren + 1);
                auto close = params.rfind(')');
                if (close != std::string::npos) params = params.substr(0, close);
                for (auto& c : params) if (c == '\n' || c == '\t' || c == '|' || c == ' ') c = ' ';
                auto new_end = std::unique(params.begin(), params.end(),
                    [](char a, char b) { return a == ' ' && b == ' '; });
                params.erase(new_end, params.end());
                if (!params.empty() && params.front() == ' ') params = params.substr(1);
                if (!params.empty() && params.back()  == ' ') params.pop_back();
            }
            strncpy_s(out.name,   fname.c_str(),  MAX_FUNC_NAME - 1);
            strncpy_s(out.owner,  owner.c_str(),  MAX_FUNC_NAME - 1);
            strncpy_s(out.params, params.c_str(), MAX_PARAM_STR - 1);
        };

        for (auto* Actor : AllActors)
        {
            if (!Actor || !IsValidRaw(Actor) || !Actor->bReplicates) continue;

            std::vector<std::string> actorFuncs;
            Scan::ScanFunctions(Actor, actorFuncs);

            TArray<UActorComponent*> Components =
                Actor->K2_GetComponentsByClass(UActorComponent::StaticClass());
            std::vector<std::pair<std::string, std::vector<std::string>>> compResults;
            for (auto* Comp : Components)
            {
                if (!Comp || !IsValidRaw(Comp)) continue;
                std::vector<std::string> compFuncs;
                Scan::ScanFunctions(Comp, compFuncs);
                if (!compFuncs.empty()) compResults.emplace_back(Comp->Class->GetName(), std::move(compFuncs));
            }
            if (actorFuncs.empty() && compResults.empty()) continue;

            ++totalActors;
            info("╔══ {} ({})", Actor->GetName(), Actor->Class->GetName());
            if (!actorFuncs.empty())
            {
                info("╠═ [actor] {} server RPCs", actorFuncs.size());
                for (size_t i = 0; i < actorFuncs.size(); ++i)
                {
                    bool last = (i == actorFuncs.size() - 1) && compResults.empty();
                    info("║   {} {}", last ? "└──" : "├──", actorFuncs[i]);
                    WriteToSr(actorFuncs[i], Actor->Class->GetName());
                    ++totalFuncs;
                }
            }
            for (size_t ci = 0; ci < compResults.size(); ++ci)
            {
                const auto& [compClass, funcs] = compResults[ci];
                bool lastComp = (ci == compResults.size() - 1);
                info("╠═ [component] {}", compClass);
                for (size_t i = 0; i < funcs.size(); ++i)
                {
                    bool last = lastComp && (i == funcs.size() - 1);
                    info("║   {} {}", last ? "└──" : "├──", funcs[i]);
                    WriteToSr(funcs[i], compClass);
                    ++totalFuncs;
                }
            }
            info("╚══════════════════════════");
        }
        info("[scanfuncs] Done — {} actors, {} server RPCs", totalActors, totalFuncs);

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
