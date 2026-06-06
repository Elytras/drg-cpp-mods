#include "Lib_GameHooks.h"
#include "Lib_ObjectFactory.h"   // FStaticConstructObjectParameters, ObjectFactory::GetStaticConstructObject
#include <unordered_set>

namespace GameHooks
{
using namespace SDK; 

// ═════════════════════════════════════════════════════════════════════════════
//  ProcessEventHook
// ═════════════════════════════════════════════════════════════════════════════

void(*ProcessEventHook::OriginalProcessEvent)(UObject*, UFunction*, void*) = nullptr;

// ── Constructor ──────────────────────────────────────────────────────────────

ProcessEventHook::ProcessEventHook()
{
    StoreEmptyState();
    DBG_ASSERT(stateOwner_.load() != nullptr, "Failed to initialize state");
}

// ── Internal helpers ─────────────────────────────────────────────────────────

void ProcessEventHook::StoreState(std::shared_ptr<const CallbackState> s)
{
    DBG_CHECK_PTR(s.get());
    s->Validate();
    const bool nonEmpty = !s->list.empty();
    std::atomic_store(&stateOwner_, std::move(s));
    hasCallbacks_.store(nonEmpty, std::memory_order_release);
    DBG_ASSERT(std::atomic_load(&stateOwner_) != nullptr, "State store failed");
}

std::shared_ptr<const ProcessEventHook::CallbackState>
ProcessEventHook::BuildState(CallbackList list)
{
    auto s = std::make_shared<CallbackState>();
    s->list = std::move(list);

    for (size_t i = 0; i < s->list.size(); ++i)
    {
        auto& e = s->list[i];
        DBG_ASSERT(e.handle > 0, "Invalid entry handle during state build");
        DBG_ASSERT(e.callback,   "Null callback during state build");

        if (e.functionFilter)
        {
            DBG_CHECK_PTR(e.functionFilter);
            s->cache.byFunctionPtr[e.functionFilter].push_back(i);
        }
        else if (!e.functionNameFilter.empty())
        {
            if (e.functionNameFName.IsNone())
                new (&e.functionNameFName) FName(ToWide(e.functionNameFilter).c_str());
            s->cache.byFunctionName[e.functionNameFName].push_back(i);
        }
        else
            s->cache.wildcards.push_back(i);
    }

    s->Validate();
    return s;
}

// ── CRTP interface ────────────────────────────────────────────────────────────

ProcessEventHook::CallbackList ProcessEventHook::CloneList() const
{
    auto s = std::atomic_load(&stateOwner_);
    if (s) { s->Validate(); return s->list; }
    return CallbackList{};
}

void ProcessEventHook::StoreList(CallbackList list)
{
    StoreState(BuildState(std::move(list)));
}

void ProcessEventHook::StoreEmptyState()
{
    StoreState(std::make_shared<CallbackState>());
}

bool ProcessEventHook::DoUninstall()
{
    return DoUninstallCommon();
}

// ── Dispatch helpers ──────────────────────────────────────────────────────────

bool ProcessEventHook::MatchesClassFilter(UObject* Object, UClass* classFilter, ClassMatchMode mode)
{
    if (!classFilter || !Object || !Object->Class)
    {
        DBG_ASSERT(classFilter || !Object || !Object->Class || mode == ClassMatchMode::Exact,
            "Missing class filter with non-Exact mode");
        return true;
    }

    DBG_CHECK_PTR(Object);
    DBG_CHECK_PTR(Object->Class);

    UClass* cls = Object->Class;
    auto& sc = SubclassCache::Get();

    switch (mode)
    {
    case ClassMatchMode::Exact:           return cls == classFilter;
    case ClassMatchMode::ExactOrSubclass: return cls == classFilter || sc.IsSubclassOf(cls, classFilter);
    case ClassMatchMode::SubclassOnly:    return cls != classFilter && sc.IsSubclassOf(cls, classFilter);
    default: DBG_ASSERT(false, "Unknown ClassMatchMode"); return false;
    }
}

bool ProcessEventHook::RunBeforePass(const CallbackList& list, const size_t* idx, size_t count,
    UObject* Object, UFunction* Function, void* Parms)
{
    DBG_CHECK_PTR(Object);
    DBG_CHECK_RANGE(count, 0, kMaxDispatchBuf);

    bool ok = true;
    bool skipFired = false;
    int  callOriginalRan = 0;

    for (size_t n = 0; n < count; ++n)
    {
        DBG_CHECK_RANGE(idx[n], 0, list.size());
        const auto& e = list[idx[n]];

        DBG_ASSERT(e.handle > 0, "Invalid callback entry handle");
        DBG_ASSERT(e.callback,   "Null callback in entry");

        if (!e.enabled || e.timing == ExecutionTiming::After)            continue;
        if (e.objectFilter && e.objectFilter != Object)                  continue;
        if (!MatchesClassFilter(Object, e.classFilter, e.classMatchMode)) continue;

        try { e.callback(Object, Function, Parms); }
        catch (const std::exception&) { DBG_ASSERT(false, "Callback threw exception"); }

        if (e.mode == ExecutionMode::SkipOriginal) { ok = false; skipFired = true; }
        else                                         ++callOriginalRan;
    }

    if (skipFired && callOriginalRan > 0)
        warn("[GameHooks] SkipOriginal suppressed the original for {} CallOriginal callback(s) on the same event (function: '{}')",
            callOriginalRan,
            Function && !Function->Name.IsNone() ? Function->Name.ToString() : "<unknown>");

    return ok;
}

void ProcessEventHook::RunAfterPass(const CallbackList& list, const size_t* idx, size_t count,
    UObject* Object, UFunction* Function, void* Parms)
{
    DBG_CHECK_PTR(Object);
    DBG_CHECK_RANGE(count, 0, kMaxDispatchBuf);

    for (size_t n = 0; n < count; ++n)
    {
        DBG_CHECK_RANGE(idx[n], 0, list.size());
        const auto& e = list[idx[n]];

        DBG_ASSERT(e.handle > 0, "Invalid callback entry handle");
        DBG_ASSERT(e.callback,   "Null callback in entry");

        if (!e.enabled || e.timing == ExecutionTiming::Before)            continue;
        if (e.objectFilter && e.objectFilter != Object)                   continue;
        if (!MatchesClassFilter(Object, e.classFilter, e.classMatchMode)) continue;

        try { e.callback(Object, Function, Parms); }
        catch (const std::exception&) { DBG_ASSERT(false, "Callback threw exception"); }
    }
}

void __fastcall ProcessEventHook::HookedProcessEvent(UObject* Object, UFunction* Function, void* Parms)
{
    if (!OriginalProcessEvent) return;

    DBG_CHECK_PTR(Object);

    int depthBefore = executionDepth_.fetch_add(1, std::memory_order_relaxed);
    DBG_CHECK_RANGE(depthBefore, 0, 1000);

    // Re-entrancy guard: if a callback is already on this thread's call stack
    // (depthBefore > 0), skip all dispatch and task-drain logic entirely.
    if (depthBefore > 0)
    {
        __assume(OriginalProcessEvent != nullptr);
        try { OriginalProcessEvent(Object, Function, Parms); }
        catch (const std::exception&) { DBG_ASSERT(false, "OriginalProcessEvent threw exception"); }
        executionDepth_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    static ProcessEventHook* inst = &Get();
    DBG_CHECK_PTR(inst);

    inst->DrainTasks();

    // Fast path: when no callbacks are registered (the common case) skip the
    // atomic<shared_ptr> load entirely — the gate is a plain atomic<bool>.
    std::shared_ptr<const CallbackState> state;
    bool hasCallbacks = false;
    if (inst->hasCallbacks_.load(std::memory_order_acquire))
    {
        state = std::atomic_load(&inst->stateOwner_);
        hasCallbacks = state && !state->list.empty();
    }

    if (hasCallbacks)
    {
        DBG_CHECK_PTR(state.get());
        state->Validate();

        size_t buf[kMaxDispatchBuf];
        size_t cnt = 0;
        auto push = [&](size_t i)
        {
            if (cnt < kMaxDispatchBuf) { DBG_CHECK_RANGE(i, 0, state->list.size()); buf[cnt++] = i; }
            else { DBG_ASSERT(false, "Callback buffer overflow"); }
        };

        if (Function)
        {
            DBG_CHECK_PTR(Function);
            if (auto it = state->cache.byFunctionPtr.find(Function);  it != state->cache.byFunctionPtr.end())
                for (size_t i : it->second) push(i);
            if (auto it = state->cache.byFunctionName.find(Function->Name); it != state->cache.byFunctionName.end())
                for (size_t i : it->second) push(i);
        }
        for (size_t i : state->cache.wildcards) push(i);

        bool callOriginal = RunBeforePass(state->list, buf, cnt, Object, Function, Parms);
        if (callOriginal)
        {
            __assume(OriginalProcessEvent != nullptr);
            try { OriginalProcessEvent(Object, Function, Parms); }
            catch (const std::exception&) { DBG_ASSERT(false, "OriginalProcessEvent threw exception"); }
        }
        RunAfterPass(state->list, buf, cnt, Object, Function, Parms);
    }
    else
    {
        __assume(OriginalProcessEvent != nullptr);
        try { OriginalProcessEvent(Object, Function, Parms); }
        catch (const std::exception&) { DBG_ASSERT(false, "OriginalProcessEvent threw exception"); }
    }

    int depthAfter = executionDepth_.fetch_sub(1, std::memory_order_acq_rel);
    DBG_CHECK_RANGE(depthAfter, 1, 1000);
    DBG_ASSERT(depthAfter > 0, "Execution depth went negative");

    if (depthAfter == 1 && inst->pendingUninstall_.load(std::memory_order_relaxed))
    {
        bool expected = true;
        if (inst->pendingUninstall_.compare_exchange_strong(expected, false))
        {
            try { inst->DoUninstall(); }
            catch (const std::exception&) { DBG_ASSERT(false, "DoUninstall threw exception"); }
        }
    }
}

// ── Public methods ────────────────────────────────────────────────────────────

// Block until the engine UObject is resolvable, then return it. Both hooks read
// their target from the engine's vtable, so only the first hook installed needs
// to wait — the engine stays up for the rest of the session.
static UEngine* WaitForEngine()
{
    UEngine* engine = nullptr;
    while (!(engine = UEngine::GetEngine())) Sleep(100);
    return engine;
}

bool ProcessEventHook::Install()
{
    if (hookInstalled_.load()) return true;
    DBG_ASSERT(!pendingUninstall_.load(), "Cannot install while uninstall pending");
    if (!EasyHook::Init()) return false;

    static void* addr = nullptr;
    if (!addr)
    {
        // ProcessEvent shares the engine's UObject vtable (slot Offsets::ProcessEventIdx).
        // By contract this runs after the EngineTick hook has already resolved the
        // engine (ModManager::LoadMods installs the tick hook first), so there's no
        // need to spin-wait for the engine here.
        UObject* engine = UEngine::GetEngine();
        if (!engine)
        {
            error("[GameHooks] ProcessEvent install before engine resolved — install the EngineTick hook first");
            return false;
        }
        addr = Utils::GetVirtualFunction<void*>(engine, Offsets::ProcessEventIdx);
        DBG_CHECK_PTR(addr);
    }

    if (!EasyHook::CreateAndEnableHook(addr, &HookedProcessEvent, &OriginalProcessEvent))
    {
        DBG_ASSERT(false, "Failed to create hook");
        return false;
    }

    DBG_CHECK_PTR(OriginalProcessEvent);
    hookAddr_ = addr;
    hookInstalled_.store(true);
    return true;
}

CallbackHandle ProcessEventHook::AddCallback(ProcessEventCallback callback,
    const std::string& functionName, UClass* classFilter,
    const UObject* objectFilter, UFunction* functionFilter,
    ClassMatchMode classMatchMode, ExecutionTiming timing, ExecutionMode mode)
{
    DBG_ASSERT(callback, "Null callback passed to AddCallback");
    std::lock_guard lock(writeMutex_);
    CallbackList newList = CloneList();
    CallbackHandle handle = nextHandle_++;
    DBG_ASSERT(handle > 0, "Invalid handle generated");
    newList.emplace_back(handle, functionName, classFilter, objectFilter,
        functionFilter, classMatchMode, timing, mode, std::move(callback));
    DBG_CHECK_RANGE(newList.size(), 1, 100000);
    StoreList(std::move(newList));
    return handle;
}

ProcessEventHook::DispatchStats ProcessEventHook::GetDispatchStats() const
{
    DispatchStats st;
    auto state = std::atomic_load(&stateOwner_);
    if (!state) return st;
    st.totalCallbacks     = state->list.size();
    st.byFunctionPtrKeys  = state->cache.byFunctionPtr.size();
    st.byFunctionNameKeys = state->cache.byFunctionName.size();
    st.wildcards          = state->cache.wildcards.size();
    for (const auto& e : state->list) if (e.enabled) ++st.enabled;
    return st;
}

// ── Free convenience functions ────────────────────────────────────────────────

bool InstallProcessEventHook() { return ProcessEventHook::Get().Install(); }

CallbackHandle OnProcessEventByName(const std::string& fn, ProcessEventCallback cb, ExecutionTiming t, ExecutionMode mode)
{
    DBG_ASSERT(!fn.empty(), "Empty function name"); DBG_ASSERT(cb, "Null callback");
    return ProcessEventHook::Get().AddCallback(std::move(cb), fn, nullptr, nullptr, nullptr, ClassMatchMode::ExactOrSubclass, t, mode);
}

CallbackHandle OnProcessEventByClass(UClass* cls, ProcessEventCallback cb, ClassMatchMode m, ExecutionTiming t, ExecutionMode mode)
{
    DBG_CHECK_PTR(cls); DBG_ASSERT(cb, "Null callback");
    return ProcessEventHook::Get().AddCallback(std::move(cb), "", cls, nullptr, nullptr, m, t, mode);
}

CallbackHandle OnProcessEventByObject(const UObject* obj, ProcessEventCallback cb, ExecutionTiming t, ExecutionMode mode)
{
    DBG_CHECK_PTR(obj); DBG_ASSERT(cb, "Null callback");
    return ProcessEventHook::Get().AddCallback(std::move(cb), "", nullptr, obj, nullptr, ClassMatchMode::ExactOrSubclass, t, mode);
}

CallbackHandle OnProcessEventByFunction(UFunction* func, ProcessEventCallback cb, ExecutionTiming t, ExecutionMode mode)
{
    DBG_CHECK_PTR(func); DBG_ASSERT(cb, "Null callback");
    return ProcessEventHook::Get().AddCallback(std::move(cb), "", nullptr, nullptr, func, ClassMatchMode::ExactOrSubclass, t, mode);
}

CallbackHandle OnProcessEventAll(ProcessEventCallback cb, ExecutionTiming t, ExecutionMode mode)
{
    DBG_ASSERT(cb, "Null callback");
    return ProcessEventHook::Get().AddCallback(std::move(cb), "", nullptr, nullptr, nullptr, ClassMatchMode::ExactOrSubclass, t, mode);
}

bool RemoveHook            (CallbackHandle h) { DBG_ASSERT(h > 0, "Invalid handle"); return ProcessEventHook::Get().RemoveCallback(h); }
bool SetHookExecutionMode  (CallbackHandle h, ExecutionMode m)   { DBG_ASSERT(h > 0, "Invalid handle"); return ProcessEventHook::Get().SetExecutionMode(h, m); }
bool SetHookExecutionTiming(CallbackHandle h, ExecutionTiming t) { DBG_ASSERT(h > 0, "Invalid handle"); return ProcessEventHook::Get().SetExecutionTiming(h, t); }

// ═════════════════════════════════════════════════════════════════════════════
//  EngineTickHook
// ═════════════════════════════════════════════════════════════════════════════

void(*EngineTickHook::OriginalTick)(UEngine*, float, bool) = nullptr;

// ── Constructor ──────────────────────────────────────────────────────────────

EngineTickHook::EngineTickHook()
{
    StoreEmptyState();
    DBG_ASSERT(stateOwner_.load() != nullptr, "Failed to initialize state");
}

// ── Internal helpers ─────────────────────────────────────────────────────────

void EngineTickHook::StoreState(std::shared_ptr<const CallbackList> s)
{
    DBG_CHECK_PTR(s.get());
    const bool nonEmpty = !s->empty();
    std::atomic_store(&stateOwner_, std::move(s));
    hasCallbacks_.store(nonEmpty, std::memory_order_release);
    DBG_ASSERT(std::atomic_load(&stateOwner_) != nullptr, "State store failed");
}

// ── CRTP interface ────────────────────────────────────────────────────────────

EngineTickHook::CallbackList EngineTickHook::CloneList() const
{
    auto s = std::atomic_load(&stateOwner_);
    return s ? *s : CallbackList{};
}

void EngineTickHook::StoreList(CallbackList list)
{
    StoreState(std::make_shared<CallbackList>(std::move(list)));
}

void EngineTickHook::StoreEmptyState()
{
    StoreState(std::make_shared<CallbackList>());
}

bool EngineTickHook::DoUninstall()
{
    // DoUninstallCommon removes only this hook's own MinHook target; EasyHook
    // releases MinHook itself once the last hook is gone (reference-counted).
    return DoUninstallCommon();
}

// ── Dispatch helpers ──────────────────────────────────────────────────────────

bool EngineTickHook::RunBeforePass(const CallbackList& list, UEngine* Eng, float Dt, bool Idle)
{
    bool ok = true;
    bool skipFired = false;
    int  callOriginalRan = 0;

    for (const auto& e : list)
    {
        if (!e.enabled || e.timing == ExecutionTiming::After) continue;
        DBG_ASSERT(e.callback, "Null callback in entry");

        try { e.callback(Eng, Dt, Idle); }
        catch (const std::exception&) { DBG_ASSERT(false, "Tick callback threw exception"); }

        if (e.mode == ExecutionMode::SkipOriginal) { ok = false; skipFired = true; }
        else                                         ++callOriginalRan;
    }

    if (skipFired && callOriginalRan > 0)
        warn("[EngineTickHook] SkipOriginal suppressed Tick for {} CallOriginal callback(s) on the same frame",
            callOriginalRan);

    return ok;
}

void EngineTickHook::RunAfterPass(const CallbackList& list, UEngine* Eng, float Dt, bool Idle)
{
    for (const auto& e : list)
    {
        if (!e.enabled || e.timing == ExecutionTiming::Before) continue;
        DBG_ASSERT(e.callback, "Null callback in entry");
        try { e.callback(Eng, Dt, Idle); }
        catch (const std::exception&) { DBG_ASSERT(false, "Tick callback threw exception"); }
    }
}

void __fastcall EngineTickHook::HookedTick(UEngine* Engine, float DeltaSeconds, bool bIdleMode)
{
    if (!OriginalTick) return;

    int depthBefore = executionDepth_.fetch_add(1, std::memory_order_relaxed);
    DBG_CHECK_RANGE(depthBefore, 0, 1000);

    static EngineTickHook* inst = &Get();
    DBG_CHECK_PTR(inst);

    if (depthBefore == 0)
        inst->DrainTasks();

    // Fast path: skip the atomic<shared_ptr> load when no tick callbacks exist.
    std::shared_ptr<const CallbackList> state;
    bool hasCallbacks = false;
    if (inst->hasCallbacks_.load(std::memory_order_acquire))
    {
        state = std::atomic_load(&inst->stateOwner_);
        hasCallbacks = state && !state->empty();
    }

    if (hasCallbacks)
    {
        DBG_CHECK_PTR(state.get());

        bool callOriginal = RunBeforePass(*state, Engine, DeltaSeconds, bIdleMode);
        if (callOriginal)
        {
            __assume(OriginalTick != nullptr);
            try { OriginalTick(Engine, DeltaSeconds, bIdleMode); }
            catch (const std::exception&) { DBG_ASSERT(false, "OriginalTick threw exception"); }
        }
        RunAfterPass(*state, Engine, DeltaSeconds, bIdleMode);
    }
    else
    {
        __assume(OriginalTick != nullptr);
        try { OriginalTick(Engine, DeltaSeconds, bIdleMode); }
        catch (const std::exception&) { DBG_ASSERT(false, "OriginalTick threw exception"); }
    }

    int depthAfter = executionDepth_.fetch_sub(1, std::memory_order_acq_rel);
    DBG_CHECK_RANGE(depthAfter, 1, 1000);

    if (depthAfter == 1 && inst->pendingUninstall_.load(std::memory_order_relaxed))
    {
        bool expected = true;
        if (inst->pendingUninstall_.compare_exchange_strong(expected, false))
        {
            try { inst->DoUninstall(); }
            catch (const std::exception&) { DBG_ASSERT(false, "DoUninstall threw exception"); }
        }
    }
}

// ── Public methods ────────────────────────────────────────────────────────────

bool EngineTickHook::Install(int32 vtableSlot)
{
    if (hookInstalled_.load()) return true;
    DBG_ASSERT(!pendingUninstall_.load(), "Cannot install while uninstall pending");
    DBG_ASSERT(vtableSlot >= 0, "vtableSlot must be a non-negative index");
    if (!EasyHook::Init()) return false;

    UEngine* engine = WaitForEngine();
    DBG_CHECK_PTR(engine);

    void* addr = Utils::GetVirtualFunction<void*>(engine, vtableSlot);
    DBG_CHECK_PTR(addr);
    if (!addr) return false;

    if (!EasyHook::CreateAndEnableHook(addr, &HookedTick, &OriginalTick))
    {
        DBG_ASSERT(false, "Failed to create EngineTick hook");
        return false;
    }

    DBG_CHECK_PTR(OriginalTick);
    hookAddr_ = addr;
    hookInstalled_.store(true);
    return true;
}

CallbackHandle EngineTickHook::AddCallback(EngineTickCallback callback,
    ExecutionTiming timing, ExecutionMode mode)
{
    DBG_ASSERT(callback, "Null callback passed to AddCallback");
    std::lock_guard lock(writeMutex_);
    CallbackList newList = CloneList();
    CallbackHandle handle = nextHandle_++;
    DBG_ASSERT(handle > 0, "Invalid handle generated");
    newList.emplace_back(handle, std::move(callback), timing, mode);
    StoreList(std::move(newList));
    return handle;
}

// ── Free convenience functions ────────────────────────────────────────────────

bool InstallEngineTickHook(int32 vtableSlot)
{
    return EngineTickHook::Get().Install(vtableSlot);
}

CallbackHandle OnEngineTick(EngineTickCallback cb, ExecutionTiming t, ExecutionMode mode)
{
    DBG_ASSERT(cb, "Null callback");
    return EngineTickHook::Get().AddCallback(std::move(cb), t, mode);
}

// ═════════════════════════════════════════════════════════════════════════════
//  StaticConstructObjectHook
// ═════════════════════════════════════════════════════════════════════════════

SDK::UObject* (*StaticConstructObjectHook::OriginalSCO)(FStaticConstructObjectParameters*) = nullptr;

// ── Constructor ──────────────────────────────────────────────────────────────

StaticConstructObjectHook::StaticConstructObjectHook()
{
    StoreEmptyState();
    DBG_ASSERT(stateOwner_.load() != nullptr, "Failed to initialize state");
}

// ── Internal helpers ─────────────────────────────────────────────────────────

void StaticConstructObjectHook::StoreState(std::shared_ptr<const CallbackList> s)
{
    DBG_CHECK_PTR(s.get());
    const bool nonEmpty = !s->empty();
    std::atomic_store(&stateOwner_, std::move(s));
    hasCallbacks_.store(nonEmpty, std::memory_order_release);
    DBG_ASSERT(std::atomic_load(&stateOwner_) != nullptr, "State store failed");
}

// ── CRTP interface ────────────────────────────────────────────────────────────

StaticConstructObjectHook::CallbackList StaticConstructObjectHook::CloneList() const
{
    auto s = std::atomic_load(&stateOwner_);
    return s ? *s : CallbackList{};
}

void StaticConstructObjectHook::StoreList(CallbackList list)
{
    StoreState(std::make_shared<CallbackList>(std::move(list)));
}

void StaticConstructObjectHook::StoreEmptyState()
{
    StoreState(std::make_shared<CallbackList>());
}

bool StaticConstructObjectHook::DoUninstall()
{
    // DoUninstallCommon removes only this hook's own MinHook target; EasyHook
    // releases MinHook itself once the last hook is gone (reference-counted).
    return DoUninstallCommon();
}

// ── Dispatch helpers ──────────────────────────────────────────────────────────

bool StaticConstructObjectHook::RunBeforePass(const CallbackList& list, FStaticConstructObjectParameters* p)
{
    bool ok = true;
    bool skipFired = false;
    int  callOriginalRan = 0;

    for (const auto& e : list)
    {
        if (!e.enabled || e.timing == ExecutionTiming::After) continue;
        DBG_ASSERT(e.callback, "Null callback in entry");
        try { e.callback(p, nullptr); }
        catch (const std::exception&) { DBG_ASSERT(false, "SCO callback threw exception"); }

        if (e.mode == ExecutionMode::SkipOriginal) { ok = false; skipFired = true; }
        else                                         ++callOriginalRan;
    }

    if (skipFired && callOriginalRan > 0)
        warn("[StaticConstructObjectHook] SkipOriginal suppressed construction for {} CallOriginal callback(s)", callOriginalRan);

    return ok;
}

void StaticConstructObjectHook::RunAfterPass(const CallbackList& list, FStaticConstructObjectParameters* p, SDK::UObject* result)
{
    for (const auto& e : list)
    {
        if (!e.enabled || e.timing == ExecutionTiming::Before) continue;
        DBG_ASSERT(e.callback, "Null callback in entry");
        try { e.callback(p, result); }
        catch (const std::exception&) { DBG_ASSERT(false, "SCO callback threw exception"); }
    }
}

SDK::UObject* StaticConstructObjectHook::HookedSCO(FStaticConstructObjectParameters* params)
{
    if (!OriginalSCO) return nullptr;

    // Per-thread re-entrancy guard. SCO runs on multiple threads (incl. async
    // loading), so a single global depth would yield cross-thread false
    // positives and drop dispatches. executionDepth_ is still bumped for
    // uninstall-safety (defer teardown until no thread is inside the trampoline).
    static thread_local int s_reentry = 0;
    const bool reentrant = (s_reentry++ > 0);
    executionDepth_.fetch_add(1, std::memory_order_relaxed);

    static StaticConstructObjectHook* inst = &Get();
    DBG_CHECK_PTR(inst);

    SDK::UObject* result = nullptr;

    if (reentrant)
    {
        result = OriginalSCO(params);
    }
    else
    {
        // Fast path: skip the atomic<shared_ptr> load when no SCO callbacks exist.
        // This is the hottest hook (fires on nearly every UObject construction),
        // so the plain atomic<bool> gate matters most here.
        std::shared_ptr<const CallbackList> state;
        bool hasCallbacks = false;
        if (inst->hasCallbacks_.load(std::memory_order_acquire))
        {
            state = std::atomic_load(&inst->stateOwner_);
            hasCallbacks = state && !state->empty();
        }

        if (hasCallbacks)
        {
            const bool callOriginal = RunBeforePass(*state, params);
            if (callOriginal)
            {
                try { result = OriginalSCO(params); }
                catch (const std::exception&) { DBG_ASSERT(false, "OriginalSCO threw exception"); }
            }
            RunAfterPass(*state, params, result);
        }
        else
        {
            result = OriginalSCO(params);
        }
    }

    --s_reentry;
    const int depthAfter = executionDepth_.fetch_sub(1, std::memory_order_acq_rel);
    DBG_CHECK_RANGE(depthAfter, 1, 1000);

    if (depthAfter == 1 && inst->pendingUninstall_.load(std::memory_order_relaxed))
    {
        bool expected = true;
        if (inst->pendingUninstall_.compare_exchange_strong(expected, false))
        {
            try { inst->DoUninstall(); }
            catch (const std::exception&) { DBG_ASSERT(false, "DoUninstall threw exception"); }
        }
    }

    return result;
}

// ── Public methods ────────────────────────────────────────────────────────────

bool StaticConstructObjectHook::Install()
{
    if (hookInstalled_.load()) return true;
    DBG_ASSERT(!pendingUninstall_.load(), "Cannot install while uninstall pending");
    if (!EasyHook::Init()) return false;

    void* addr = reinterpret_cast<void*>(ObjectFactory::GetStaticConstructObject());
    if (!addr)
    {
        // Unresolved for this game (e.g. RC until GameOffsets is filled in).
        // ObjectFactory already logged the reason once.
        return false;
    }

    if (!EasyHook::CreateAndEnableHook(addr, &HookedSCO, &OriginalSCO))
    {
        DBG_ASSERT(false, "Failed to create StaticConstructObject hook");
        return false;
    }

    DBG_CHECK_PTR(OriginalSCO);
    hookAddr_ = addr;
    hookInstalled_.store(true);
    info("[StaticConstructObjectHook] Installed @ {:p}", addr);
    return true;
}

CallbackHandle StaticConstructObjectHook::AddCallback(StaticConstructObjectCallback callback,
    ExecutionTiming timing, ExecutionMode mode)
{
    DBG_ASSERT(callback, "Null callback passed to AddCallback");
    std::lock_guard lock(writeMutex_);
    CallbackList newList = CloneList();
    CallbackHandle handle = nextHandle_++;
    DBG_ASSERT(handle > 0, "Invalid handle generated");
    newList.emplace_back(handle, std::move(callback), timing, mode);
    StoreList(std::move(newList));
    return handle;
}

// ── Free convenience functions ────────────────────────────────────────────────

bool InstallStaticConstructObjectHook()
{
    return StaticConstructObjectHook::Get().Install();
}

CallbackHandle OnStaticConstructObject(StaticConstructObjectCallback cb, ExecutionTiming t, ExecutionMode mode)
{
    DBG_ASSERT(cb, "Null callback");
    return StaticConstructObjectHook::Get().AddCallback(std::move(cb), t, mode);
}

// ═════════════════════════════════════════════════════════════════════════════
//  HookToggle registry
// ═════════════════════════════════════════════════════════════════════════════

// Leaked (never destroyed) so toggle destructors at process/DLL exit always find
// a live registry — same philosophy as HookBase::Get()'s leaked singleton.
namespace {
    std::mutex& gToggleMutex()
    {
        static std::mutex* m = new std::mutex();
        return *m;
    }
    std::unordered_set<ToggleBase*>& gToggleRegistry()
    {
        static auto* s = new std::unordered_set<ToggleBase*>();
        return *s;
    }
}

ToggleBase::ToggleBase()
{
    std::lock_guard<std::mutex> lock(gToggleMutex());
    gToggleRegistry().insert(this);
}

ToggleBase::~ToggleBase()
{
    std::lock_guard<std::mutex> lock(gToggleMutex());
    gToggleRegistry().erase(this);
}

void ResetAllToggles() noexcept
{
    std::lock_guard<std::mutex> lock(gToggleMutex());
    for (auto* t : gToggleRegistry())
        if (t) t->Reset();
}

} // namespace GameHooks

void EnqueueOnce(std::function<void()> fn)
{
    GameHooks::EngineTickHook::Get().Enqueue(std::move(fn));
}

void EnqueueWhile(std::function<bool()> fn)
{
    auto& hook = GameHooks::EngineTickHook::Get();
    auto task = std::make_shared<std::function<void()>>();
    *task = [fn = std::move(fn), task, &hook]() mutable
        {
            if (fn()) hook.Enqueue(*task);
        };
    hook.Enqueue(*task);
}

GameHooks::CallbackHandle EnqueueEveryNTicks(
    uint32_t                   n,
    std::function<bool()>      fn,
    GameHooks::ExecutionTiming timing)
{
    if (n == 0) n = 1;

    // The handle is heap-allocated so the lambda can capture it by shared_ptr
    // and self-remove once fn returns false — without knowing the handle value
    // at construction time (OnEngineTick hasn't returned yet when the lambda is
    // built, so the actual handle is written into *handle after the call).
    auto counter = std::make_shared<uint32_t>(0);
    auto handle  = std::make_shared<GameHooks::CallbackHandle>(0);

    *handle = GameHooks::OnEngineTick(
        [n, fn = std::move(fn), counter, handle](SDK::UEngine*, float, bool) mutable
        {
            if (++(*counter) < n) return;
            *counter = 0;
            if (!fn())
                GameHooks::EngineTickHook::Get().RemoveCallback(*handle);
        },
        timing);

    return *handle;
}
