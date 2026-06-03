// SharedCommands.cpp — game-agnostic commands shared by DrgMods and RcMods.
//
// Compiled once per consumer (DrgMods / RcMods) against that game's Library.h,
// like ModManager.cpp. Everything here touches only SDK symbols that exist with
// identical names in both games' generated SDKs, so the same TU compiles in both.
#include "SharedCommands.h"
#include "Library.h"
#include "Lib_NetLogConfig.h"
#include "Commands.h"          // per-game policy hook: OnWorldChanged()

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <memory>
#include <cctype>
#include <cstdint>
#include <cstdlib>

// File-local SDK pollution: this TU uses no math wrappers (FVector etc.), so
// unqualified SDK lookup is unambiguous — same reasoning as ModManager.cpp.
using namespace SDK;
using namespace VarSystem;

namespace
{
    // A dummy context for commands that re-invoke other commands internally.
    // CommandContext::args is a reference, so it must bind to a real object that
    // outlives the context — not a temporary.
    const std::vector<std::string> s_emptyArgs{};
    const CommandContext           s_dummyCtx{ s_emptyArgs };

    // Shared parameter pretty-printer: "Name=Value, Name2=Value2" over a
    // UFunction's Parm properties. Reused by the net loggers (and netfreq later).
    std::string BuildParamString(UFunction* Function, void* Params)
    {
        std::string out;
        if (!Function || !Params) return out;
        bool first = true;
        for (FField* field : FFieldRange(Function->ChildProperties))
        {
            if (!FieldCast::IsA<FProperty>(field)) continue;
            auto* prop = static_cast<FProperty*>(field);
            const auto pf = static_cast<EPropertyFlags>(prop->PropertyFlags);
            if (!(pf & EPropertyFlags::Parm))      continue;
            if (  pf & EPropertyFlags::ReturnParm) continue;
            if (!first) out += ", ";
            first = false;
            out += prop->Name.ToString();
            out += '=';
            out += GetFieldValueAsString(reinterpret_cast<uintptr_t>(Params), field);
        }
        return out;
    }

    // Build the unified skip set from config.yaml (re-read on every (re-)enable so
    // edits take effect without a restart).
    std::unordered_set<FName> LoadNetSkipList(const char* tag)
    {
        const auto cfg = NetLogConfig::Load();
        std::unordered_set<FName> skipList;
        skipList.reserve(cfg.netSkip.size());
        for (const auto& s : cfg.netSkip)
            skipList.emplace(StringLib::ToWide(s).c_str());
        info("[cmd:{}] skip list: {} entr{}",
             tag, skipList.size(), skipList.size() == 1 ? "y" : "ies");
        return skipList;
    }

    // ── lognetclient / lognetserver / reloadnetlog ───────────────────────────
    GameHooks::HookToggle<GameHooks::ProcessEventHook> s_logNetClient{
        [] {
            auto skipList = LoadNetSkipList("lognetclient");
            return GameHooks::OnProcessEventAll(
                [skipList = std::move(skipList)](UObject* Object, UFunction* Function, void* Params)
                {
                    if (!Object || !Function) return;
                    const auto ff = static_cast<EFunctionFlags>(Function->FunctionFlags);
                    if ((!(ff & EFunctionFlags::NetClient) &&
                         !(ff & EFunctionFlags::NetMulticast)) ||
                        skipList.count(Function->Name) > 0) return;

                    const std::string callerClass = Object->Class ? Object->Class->GetName() : "?";
                    const std::string callerName  = Object->GetName();

                    UObject* outer = Object->Outer;
                    const bool hasOwner = outer && outer->Class &&
                        outer->IsA(AActor::StaticClass()) && outer != Object;

                    const bool  isMulticast = static_cast<bool>(ff & EFunctionFlags::NetMulticast);
                    const char* tag         = isMulticast ? "Multicast" : "NetClient";
                    const std::string paramStr = BuildParamString(Function, Params);

                    if (hasOwner)
                        info("[{}] {}({}) | caller: {}::{} (owner: {})",
                             tag, Function->GetName(), paramStr,
                             callerClass, callerName, outer->GetName());
                    else
                        info("[{}] {}({}) | caller: {}::{}",
                             tag, Function->GetName(), paramStr, callerClass, callerName);
                },
                GameHooks::ExecutionTiming::Before);
        }
    };

    void LogNetClient(const CommandContext& = s_dummyCtx)
    {
        info("[cmd:lognetclient] {}", s_logNetClient.Toggle()
            ? "enabled — logging all NetClient + NetMulticast RPCs" : "disabled");
    }

    GameHooks::HookToggle<GameHooks::ProcessEventHook> s_logNetServer{
        [] {
            auto skipList = LoadNetSkipList("lognetserver");
            return GameHooks::OnProcessEventAll(
                [skipList = std::move(skipList)](UObject* Object, UFunction* Function, void* Params)
                {
                    if (!Object || !Function) return;
                    const auto ff = static_cast<EFunctionFlags>(Function->FunctionFlags);
                    if (!(ff & EFunctionFlags::NetServer) ||
                        skipList.count(Function->Name) > 0) return;

                    const std::string callerClass = Object->Class ? Object->Class->GetName() : "?";
                    const std::string callerName  = Object->GetName();

                    UObject* outer = Object->Outer;
                    const bool hasOwner = outer && outer->Class &&
                        outer->IsA(AActor::StaticClass()) && outer != Object;

                    const std::string paramStr = BuildParamString(Function, Params);

                    if (hasOwner)
                        info("[NetServer] {}({}) | caller: {}::{} (owner: {})",
                             Function->GetName(), paramStr,
                             callerClass, callerName, outer->GetName());
                    else
                        info("[NetServer] {}({}) | caller: {}::{}",
                             Function->GetName(), paramStr, callerClass, callerName);
                },
                GameHooks::ExecutionTiming::Before);
        }
    };

    void LogNetServer(const CommandContext& = s_dummyCtx)
    {
        info("[cmd:lognetserver] {}", s_logNetServer.Toggle()
            ? "enabled — logging all NetServer RPCs" : "disabled");
    }

    void ReloadNetLog(const CommandContext& = s_dummyCtx)
    {
        const bool clientOn = s_logNetClient.IsEnabled();
        const bool serverOn = s_logNetServer.IsEnabled();
        if (!clientOn && !serverOn)
        {
            info("[cmd:reloadnetlog] neither logger is active — nothing to reload");
            return;
        }
        // Toggle off then on for each active logger so the fresh config is picked up.
        if (clientOn) { LogNetClient(s_dummyCtx); LogNetClient(s_dummyCtx); }
        if (serverOn) { LogNetServer(s_dummyCtx); LogNetServer(s_dummyCtx); }
    }

    // ── scanall ──────────────────────────────────────────────────────────────
    void ScanAll(const CommandContext& = s_dummyCtx)
    {
        Scan::ScanAllClasses();
    }

    // ── exec ─────────────────────────────────────────────────────────────────
    // Named ExecCmd (not Exec) to avoid clashing with the global ::Exec(std::string)
    // utility from Lib_Utils.h, which it forwards to.
    void ExecCmd(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 2) { warn("[cmd:exec] usage: exec <command>"); return; }
        std::string cmd;
        for (size_t i = 1; i < ctx.args.size(); ++i) { if (i > 1) cmd += ' '; cmd += ctx.args[i]; }
        ::Exec(cmd);
    }

    // ── findclass ────────────────────────────────────────────────────────────
    void FindClass(const CommandContext& ctx)
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

    // ── logallevents ─────────────────────────────────────────────────────────
    GameHooks::HookToggle<GameHooks::ProcessEventHook> s_logAllEvents{
        [] { return GameHooks::OnProcessEventAll(
            [](UObject* Object, UFunction* Function, void*)
            {
                if (!Object || !Function) return;
                info("[PE] {}::{}",
                    Object->Class ? Object->Class->GetName() : "?",
                    Function->GetName());
            }); } };

    void LogAllEvents(const CommandContext& = s_dummyCtx)
    {
        info("[cmd:logallevents] {}", s_logAllEvents.Toggle()
            ? "enabled (every ProcessEvent will be logged)" : "disabled");
    }

    // ── stoptick ─────────────────────────────────────────────────────────────
    GameHooks::HookToggle<GameHooks::ProcessEventHook> s_tickWatcher{
        [] { return GameHooks::OnProcessEventByNameAndClass(
            "ReceiveTick", AActor::StaticClass(),
            [](UObject* Object, UFunction*, void*)
            {
                if (Object) info("[ReceiveTick] {}", Object->GetName());
            }); } };

    void StopTick(const CommandContext& = s_dummyCtx)
    {
        info("[cmd:stoptick] tick listener {}", s_tickWatcher.Toggle() ? "enabled" : "disabled");
    }

    // ── netfreq: per-function net-RPC frequency aggregation ──────────────────
    // Counts net-RPC ProcessEvent calls per UFunction instead of streaming one log
    // line each, and dumps a sorted table on every world change (and on disable).
    // ALL mutable state below is touched only on the game thread: the ProcessEvent
    // accumulator and the EngineTick world-watcher both run there, and the command
    // body defers its state changes via EnqueueOnce — so no locking is required.
    enum class NetFreqScope { All, Client, Server };

    struct FnStat
    {
        uint64_t    count    = 0;   // calls during the current world (reset on flush)
        uint64_t    lifetime = 0;   // calls since netfreq was enabled
        std::string callerClass;    // last-seen caller class
        std::string lastParams;     // most recent argument string
        std::unordered_map<std::string, uint64_t> paramFreq;  // arg string -> occurrences
    };

    constexpr size_t                  kNetFreqMaxParamVariants = 256;
    bool                              s_netFreqEnabled = false;
    NetFreqScope                      s_netFreqScope   = NetFreqScope::All;
    size_t                            s_netFreqTopN    = 25;   // 0 == uncapped
    std::unordered_set<FName>         s_netFreqSkip;
    std::unordered_map<FName, FnStat> s_netFreqStats;
    GameHooks::CallbackHandle         s_netFreqHandle  = 0;

    void NetFreqFlush(const char* reason)
    {
        if (s_netFreqStats.empty())
        {
            info("[netfreq] {} — no calls recorded", reason);
            return;
        }

        std::vector<std::pair<FName, const FnStat*>> rows;
        rows.reserve(s_netFreqStats.size());
        uint64_t total = 0;
        for (const auto& [fn, st] : s_netFreqStats) { rows.emplace_back(fn, &st); total += st.count; }
        std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.second->count > b.second->count; });

        const size_t shown = (s_netFreqTopN == 0) ? rows.size()
                                                  : (std::min)(rows.size(), s_netFreqTopN);
        if (s_netFreqTopN != 0 && rows.size() > shown)
            info("[netfreq] {} — {} fn(s), {} call(s) (top {})", reason, rows.size(), total, shown);
        else
            info("[netfreq] {} — {} fn(s), {} call(s)", reason, rows.size(), total);

        for (size_t i = 0; i < shown; ++i)
        {
            const FName&  fn = rows[i].first;
            const FnStat& st = *rows[i].second;
            const std::string* best = nullptr; uint64_t bestN = 0;
            for (const auto& [p, n] : st.paramFreq)
                if (n > bestN) { bestN = n; best = &p; }

            info("  {:>6}  {}::{}  last[{}]  most[{}x: {}]",
                 st.count, st.callerClass, fn.ToString(),
                 st.lastParams, bestN, best ? *best : std::string{});
        }

        // Reset per-world counters; keep lifetime totals across worlds.
        for (auto& [fn, st] : s_netFreqStats) { st.count = 0; st.paramFreq.clear(); }
    }

    GameHooks::CallbackHandle RegisterNetFreqAccumulator()
    {
        return GameHooks::OnProcessEventAll(
            [](UObject* Object, UFunction* Function, void* Params)
            {
                if (!s_netFreqEnabled || !Object || !Function) return;
                const auto ff = static_cast<EFunctionFlags>(Function->FunctionFlags);
                const bool isClient = static_cast<bool>(ff & EFunctionFlags::NetClient)
                                   || static_cast<bool>(ff & EFunctionFlags::NetMulticast);
                const bool isServer = static_cast<bool>(ff & EFunctionFlags::NetServer);
                bool match = false;
                switch (s_netFreqScope)
                {
                    case NetFreqScope::All:    match = isClient || isServer; break;
                    case NetFreqScope::Client: match = isClient;             break;
                    case NetFreqScope::Server: match = isServer;             break;
                }
                if (!match || s_netFreqSkip.count(Function->Name) > 0) return;

                FnStat& st = s_netFreqStats[Function->Name];
                ++st.count;
                ++st.lifetime;
                st.callerClass = Object->Class ? Object->Class->GetName() : "?";
                st.lastParams  = BuildParamString(Function, Params);
                // Bound per-function param-variant memory: keep counting strings we
                // already track, but stop inserting new variants past the cap.
                if (st.paramFreq.size() < kNetFreqMaxParamVariants ||
                    st.paramFreq.find(st.lastParams) != st.paramFreq.end())
                    ++st.paramFreq[st.lastParams];
            },
            GameHooks::ExecutionTiming::Before);
    }

    void NetFreq(const CommandContext& ctx)
    {
        // Parse args on the calling thread (pure local work).
        NetFreqScope scope = NetFreqScope::All;
        size_t       topN  = 25;
        if (ctx.ArgCount() >= 2)
        {
            std::string s = ctx.Arg(1);
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if      (s == "client") scope = NetFreqScope::Client;
            else if (s == "server") scope = NetFreqScope::Server;
            else if (s == "all")    scope = NetFreqScope::All;
            else { warn("[cmd:netfreq] unknown scope '{}'; use client|server|all", s); return; }
        }
        if (ctx.ArgCount() >= 3)
        {
            const std::string& t = ctx.Arg(2);
            topN = (t == "all") ? 0 : static_cast<size_t>(std::strtoul(t.c_str(), nullptr, 10));
        }

        // Build the skip set here (worker thread — same as the netlog toggles do).
        auto skip = std::make_shared<std::unordered_set<FName>>();
        for (const auto& s : NetLogConfig::Load().netSkip)
            skip->emplace(StringLib::ToWide(s).c_str());

        // Apply every state change on the game thread so the accumulator (also game
        // thread) never races the stats map or the config.
        EnqueueOnce([scope, topN, skip]()
        {
            if (s_netFreqEnabled)
            {
                NetFreqFlush("disabled");
                if (s_netFreqHandle) { GameHooks::RemoveHook(s_netFreqHandle); s_netFreqHandle = 0; }
                s_netFreqEnabled = false;
                s_netFreqStats.clear();
                info("[cmd:netfreq] disabled");
                return;
            }

            s_netFreqScope   = scope;
            s_netFreqTopN    = topN;
            s_netFreqSkip    = std::move(*skip);
            s_netFreqStats.clear();
            s_netFreqEnabled = true;
            s_netFreqHandle  = RegisterNetFreqAccumulator();

            const char* scopeName = scope == NetFreqScope::Client ? "client"
                                  : scope == NetFreqScope::Server ? "server" : "all";
            info("[cmd:netfreq] enabled — scope={}, top={}, flush on world change ({} skip entr{})",
                 scopeName, topN == 0 ? std::string("uncapped") : std::to_string(topN),
                 s_netFreqSkip.size(), s_netFreqSkip.size() == 1 ? "y" : "ies");
        });
    }

    // ── pewatch: keyword-filtered ProcessEvent logger (with arguments) ────────
    // logallevents narrowed to a subsystem, plus the call's argument values.
    // Logs any ProcessEvent whose function / class / object name matches ANY
    // supplied keyword (case-insensitive substring), printing parameters like the
    // net loggers do. Built for reverse-engineering an interaction:
    //   pewatch use usable            → the whole use/interact chain (local + net)
    //   pewatch widget menu viewport  → where a UI gets created and shown
    //   pewatch InstantUsable         → every call on a specific named subobject
    // The keyword list is captured by value into the callback (lock-free dispatch);
    // s_peWatchHandle is only touched on the command thread. Verbose — debug only.
    GameHooks::CallbackHandle s_peWatchHandle = 0;

    void PeWatch(const CommandContext& ctx)
    {
        std::vector<std::string> filters;
        for (size_t i = 1; i < ctx.args.size(); ++i)
            if (!ctx.args[i].empty()) filters.push_back(ctx.args[i]);

        if (filters.empty())
        {
            if (s_peWatchHandle)
            {
                GameHooks::RemoveHook(s_peWatchHandle);
                s_peWatchHandle = 0;
                info("[cmd:pewatch] disabled");
            }
            else
                warn("[cmd:pewatch] usage: pewatch <keyword> [keyword...] — matches "
                     "function/class/object name; run with no args to stop");
            return;
        }

        std::string joined;
        for (size_t i = 0; i < filters.size(); ++i) { if (i) joined += ", "; joined += filters[i]; }

        // Reconfigure cleanly if a watcher is already installed.
        if (s_peWatchHandle) { GameHooks::RemoveHook(s_peWatchHandle); s_peWatchHandle = 0; }

        s_peWatchHandle = GameHooks::OnProcessEventAll(
            [filters = std::move(filters)](UObject* Object, UFunction* Function, void* Params)
            {
                if (!Object || !Function) return;
                const std::string funcName  = Function->GetName();
                const std::string className = Object->Class ? Object->Class->GetName() : "?";
                const std::string objName   = Object->GetName();

                bool match = false;
                for (const auto& kw : filters)
                    if (PropertyInspector::NameMatches(funcName,  kw, true) ||
                        PropertyInspector::NameMatches(className, kw, true) ||
                        PropertyInspector::NameMatches(objName,   kw, true))
                    { match = true; break; }
                if (!match) return;

                const auto ff  = static_cast<EFunctionFlags>(Function->FunctionFlags);
                auto       has = [&](EFunctionFlags f) { return static_cast<bool>(ff & f); };
                const char* net = has(EFunctionFlags::NetMulticast) ? "Multicast"
                                : has(EFunctionFlags::NetServer)    ? "NetServer"
                                : has(EFunctionFlags::NetClient)    ? "NetClient" : "local";
                const std::string paramStr = BuildParamString(Function, Params);

                UObject* outer = Object->Outer;
                if (outer && outer != Object)
                    info("[pe:{}] {}::{}({}) | obj: {} (outer: {})",
                         net, className, funcName, paramStr, objName, outer->GetName());
                else
                    info("[pe:{}] {}::{}({}) | obj: {}",
                         net, className, funcName, paramStr, objName);
            },
            GameHooks::ExecutionTiming::Before);

        info("[cmd:pewatch] enabled — matching function/class/object name against: {}", joined);
    }

    // ── dumpfuncs: list every UFunction on a class (or an object's class) ─────
    // scanall/scanfuncs only surface NetServer RPCs; this dumps ALL functions —
    // local, BlueprintCallable, Exec, native, every net kind — across the full
    // class hierarchy, with a flag tag per entry. Resolves <name> as a UClass
    // (exact name preferred, then fuzzy), else the class of the first matching
    // object. Optional trailing keyword filters the listed functions.
    void DumpFuncs(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 2)
        {
            warn("[cmd:dumpfuncs] usage: dumpfuncs <class-or-object-name> [name-filter]");
            return;
        }
        const std::string& needle = ctx.Arg(1);
        const std::string  filter = ctx.ArgCount() >= 3 ? ctx.Arg(2) : std::string{};

        auto findClass = [&](bool fuzzy) -> UClass*
        {
            for (int i = 0; i < UObject::GObjects->Num(); ++i)
            {
                UObject* obj = UObject::GObjects->GetByIndex(i);
                if (!obj || !obj->IsA(UClass::StaticClass())) continue;
                if (PropertyInspector::NameMatches(obj->GetName(), needle, fuzzy))
                    return static_cast<UClass*>(obj);
            }
            return nullptr;
        };

        UClass*     targetClass = nullptr;
        std::string resolvedFrom;
        if (UClass* c = findClass(false))      { targetClass = c; resolvedFrom = "exact class"; }
        else if (UClass* c2 = findClass(true)) { targetClass = c2; resolvedFrom = "fuzzy class"; }
        else
            for (int i = 0; i < UObject::GObjects->Num() && !targetClass; ++i)
            {
                UObject* obj = UObject::GObjects->GetByIndex(i);
                if (!obj || !obj->Class) continue;
                if (PropertyInspector::NameMatches(obj->GetName(), needle, true))
                {
                    targetClass  = obj->Class;
                    resolvedFrom = "object " + obj->GetName();
                }
            }
        if (!targetClass) { warn("[cmd:dumpfuncs] no class or object matches '{}'", needle); return; }

        info("[cmd:dumpfuncs] {} (via {}){}", targetClass->GetName(), resolvedFrom,
             filter.empty() ? std::string{} : (" — filter '" + filter + "'"));

        int shown = 0, total = 0;
        for (UClass* cls : UClassHierarchyRange(targetClass))
        {
            bool headerShown = false;
            for (auto* field : UFieldRange(cls->Children))
            {
                if (!field->IsA(EClassCastFlags::Function)) continue;
                auto* Func = static_cast<UFunction*>(field);
                ++total;
                if (!filter.empty() && !PropertyInspector::NameMatches(Func->GetName(), filter, true)) continue;

                if (!headerShown) { info("╔══ {}", cls->GetName()); headerShown = true; }

                const auto ff  = static_cast<EFunctionFlags>(Func->FunctionFlags);
                auto       has = [&](EFunctionFlags f) { return static_cast<bool>(ff & f); };
                std::string tags;
                if (has(EFunctionFlags::NetServer))         tags += "NetServer ";
                if (has(EFunctionFlags::NetClient))         tags += "NetClient ";
                if (has(EFunctionFlags::NetMulticast))      tags += "Multicast ";
                if (has(EFunctionFlags::Exec))              tags += "Exec ";
                if (has(EFunctionFlags::BlueprintCallable)) tags += "BPCallable ";
                if (has(EFunctionFlags::Native))            tags += "Native ";
                info("║   {}{}", Scan::BuildFuncSig(Func),
                     tags.empty() ? std::string{} : ("  [" + tags + "]"));
                ++shown;
            }
        }
        info("[cmd:dumpfuncs] {} function(s){}", shown,
             filter.empty() ? " total across hierarchy"
                            : (" match '" + filter + "' of " + std::to_string(total)));
    }

} // anonymous namespace

void RegisterSharedCommands(CommandHandler& handler)
{
    // System
    handler.Register("lognetclient", LogNetClient, "System", R"(Toggle logging of all NetClient + NetMulticast ProcessEvent calls)");
    handler.Register("lognetserver", LogNetServer, "System", R"(Toggle logging of all NetServer (server RPC) ProcessEvent calls)");
    handler.Register("reloadnetlog", ReloadNetLog, "System", R"(Reload config.yaml skip lists without toggling active loggers off manually)");
    handler.Register("exec",         ExecCmd,      "System", R"(Execute a console command: exec <command>)");
    handler.Register("logallevents", LogAllEvents, "System", R"(Toggle logging of ALL ProcessEvent calls (very verbose))");
    handler.Register("stoptick",     StopTick,     "System", R"(Toggle ReceiveTick event logging on AActor)");
    handler.Register("netfreq",      NetFreq,      "System", R"(Toggle net-RPC frequency aggregation: netfreq [client|server|all] [topN] — counts calls per RPC and dumps a sorted table on world change; topN 0/all = uncapped)");
    handler.Register("pewatch",      PeWatch,      "System", R"(Keyword-filtered ProcessEvent logger with arguments: pewatch <kw> [kw...] — logs calls whose function/class/object name matches any keyword (run with no args to stop))");

    // Inspection
    handler.Register("findclass", FindClass, "Inspection", R"(Find classes by name (fuzzy substring))");
    handler.Register("scanall",   ScanAll,   "Inspection", R"(Scan all classes for server RPCs and log them)");
    handler.Register("dumpfuncs", DumpFuncs, "Inspection", R"(List every UFunction on a class/object across its hierarchy with flag tags: dumpfuncs <class-or-object> [name-filter])");

    // Variables (implementations live in Lib_VarSystem)
    handler.Register("get",   Cmd_Get,   "Variables", R"(Get a variable: get <n>)");
    handler.Register("set",   Cmd_Set,   "Variables", R"(Set a variable: set <n> <value>)");
    handler.Register("unset", Cmd_Unset, "Variables", R"(Delete a variable: unset <n>)");
    handler.Register("vars",  Cmd_Vars,  "Variables", R"(List all variables)");
}

void InitSharedCallbacks()
{
    static bool installed = false;
    if (installed) return;
    installed = true;

    // Always-on world-change watcher (game thread). On every transition of the
    // active UWorld pointer it invalidates per-game world-scoped caches via the
    // OnWorldChanged() policy hook, and flushes the netfreq table if active.
    GameHooks::OnEngineTick(
        [](SDK::UEngine*, float, bool)
        {
            static SDK::UWorld* lastWorld = nullptr;
            SDK::UWorld* cur = SDK::UWorld::GetWorld();
            if (cur == lastWorld) return;
            if (lastWorld != nullptr)            // ignore the first world acquisition
            {
                OnWorldChanged();
                if (s_netFreqEnabled) NetFreqFlush("world change");
            }
            lastWorld = cur;
        });
}
