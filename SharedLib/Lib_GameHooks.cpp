#include "Lib_GameHooks.h"

namespace GameHooks
{
using namespace SDK;  // file-local: this TU has no math types, no leak risk

// ── Static data member definitions ──────────────────────────────────────────

ProcessEventHook* ProcessEventHook::instance                                     = nullptr;
void(*ProcessEventHook::OriginalProcessEvent)(UObject*, UFunction*, void*)       = nullptr;

// ── Constructor ──────────────────────────────────────────────────────────────

ProcessEventHook::ProcessEventHook()
{
    auto e = std::make_shared<CallbackState>();
    StoreState(e);
    DBG_ASSERT(stateOwner_.load() != nullptr, "Failed to initialize state");
}

// ── Private helpers ──────────────────────────────────────────────────────────

void ProcessEventHook::StoreState(std::shared_ptr<const CallbackState> s)
{
    DBG_CHECK_PTR(s.get());
    s->Validate();
    std::atomic_store(&stateOwner_, std::move(s));
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
                e.functionNameFName = FName(ToWide(e.functionNameFilter).c_str());
            s->cache.byFunctionName[e.functionNameFName].push_back(i);
        }
        else
            s->cache.wildcards.push_back(i);
    }

    s->Validate();
    return s;
}

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
    case ClassMatchMode::Exact:          return cls == classFilter;
    case ClassMatchMode::ExactOrSubclass:return cls == classFilter || sc.IsSubclassOf(cls, classFilter);
    case ClassMatchMode::SubclassOnly:   return cls != classFilter && sc.IsSubclassOf(cls, classFilter);
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

        if (!e.enabled || e.timing == ExecutionTiming::After)          continue;
        if (e.objectFilter && e.objectFilter != Object)                 continue;
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

        if (!e.enabled || e.timing == ExecutionTiming::Before)         continue;
        if (e.objectFilter && e.objectFilter != Object)                 continue;
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

    static ProcessEventHook* inst = &Get();
    DBG_CHECK_PTR(inst);

    if (inst->hasTasks_.load(std::memory_order_acquire))
    {
        std::vector<std::function<void()>> local;
        {
            std::lock_guard lock(inst->taskMutex_);
            std::swap(local, inst->taskQueue_);
            inst->hasTasks_.store(false, std::memory_order_relaxed);
        }
        while (!local.empty())
        {
            DBG_ASSERT(local.back(), "Null task in queue");
            try { local.back()(); }
            catch (const std::exception&) { DBG_ASSERT(false, "Task threw exception"); }
            local.pop_back();
        }
    }

    auto state = std::atomic_load(&inst->stateOwner_);
    bool hasCallbacks = state && !state->list.empty();

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
        // No callbacks registered (or no state yet): pure passthrough.
        // ANY OTHER PATH that doesn't call OriginalProcessEvent silently
        // swallows every PE call — that's the bug that made RcMods freeze
        // the game world while DrgMods worked (DRG always has callbacks).
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

bool ProcessEventHook::DoUninstall()
{
    DBG_ASSERT(!hookInstalled_.load() || pendingUninstall_.load(), "Uninstall without pending flag");

    { std::lock_guard lock(writeMutex_); StoreState(std::make_shared<CallbackState>()); }

    DBG_ASSERT(executionDepth_.load(std::memory_order_relaxed) == 0, "DoUninstall called with non-zero execution depth");

    EasyHook::Shutdown();
    hookInstalled_.store(false);
    DBG_ASSERT(!hookInstalled_.load(), "Failed to clear hookInstalled flag");

    if (onUninstalled_)
    {
        auto cb = std::move(onUninstalled_);
        onUninstalled_ = nullptr;
        try { cb(); }
        catch (const std::exception&) { DBG_ASSERT(false, "onUninstalled callback threw exception"); }
    }
    return true;
}

ProcessEventHook::CallbackList ProcessEventHook::CloneList() const
{
    auto s = std::atomic_load(&stateOwner_);
    if (s) { s->Validate(); return s->list; }
    return CallbackList{};
}

// ── Public methods ───────────────────────────────────────────────────────────

ProcessEventHook& ProcessEventHook::Get()
{
    if (!instance) instance = new ProcessEventHook();
    DBG_CHECK_PTR(instance);
    return *instance;
}

bool ProcessEventHook::IsInstalled() const
{
    return hookInstalled_.load();
}

bool ProcessEventHook::Install()
{
    if (hookInstalled_.load()) return true;
    DBG_ASSERT(!pendingUninstall_.load(), "Cannot install while uninstall pending");
    if (!EasyHook::Init()) return false;

    static void* addr = nullptr;
    if (!addr)
    {
        UObject* engine = nullptr;
        while (!(engine = UEngine::GetEngine())) Sleep(100);
        DBG_CHECK_PTR(engine);
        addr = Utils::GetVirtualFunction<void*>(engine, Offsets::ProcessEventIdx);
        DBG_CHECK_PTR(addr);
    }

    if (!EasyHook::CreateAndEnableHook(addr, &HookedProcessEvent, &OriginalProcessEvent))
    {
        DBG_ASSERT(false, "Failed to create hook");
        return false;
    }

    DBG_CHECK_PTR(OriginalProcessEvent);
    hookInstalled_.store(true);
    return true;
}

bool ProcessEventHook::RequestUninstall()
{
    int depth = executionDepth_.load();
    DBG_CHECK_RANGE(depth, 0, 1000);
    if (depth > 0) { pendingUninstall_ = true; return true; }
    return DoUninstall();
}

void ProcessEventHook::SetOnUninstalled(std::function<void()> cb)
{
    DBG_ASSERT(cb, "Null callback passed to SetOnUninstalled");
    onUninstalled_ = std::move(cb);
}

void ProcessEventHook::Enqueue(std::function<void()> task)
{
    DBG_ASSERT(task, "Null task passed to Enqueue");
    std::lock_guard lock(taskMutex_);
    taskQueue_.push_back(std::move(task));
    DBG_CHECK_RANGE(taskQueue_.size(), 1, 100000);
    hasTasks_.store(true, std::memory_order_release);
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
    StoreState(BuildState(std::move(newList)));
    return handle;
}

bool ProcessEventHook::RemoveCallback(CallbackHandle handle)
{
    DBG_ASSERT(handle > 0, "Invalid handle passed to RemoveCallback");
    std::lock_guard lock(writeMutex_);
    CallbackList newList = CloneList();
    auto it = std::remove_if(newList.begin(), newList.end(),
        [handle](const CallbackEntry& e) { return e.handle == handle; });
    if (it == newList.end()) return false;
    newList.erase(it, newList.end());
    StoreState(BuildState(std::move(newList)));
    return true;
}

bool ProcessEventHook::EnableCallback(CallbackHandle handle, bool enabled)
{
    DBG_ASSERT(handle > 0, "Invalid handle passed to EnableCallback");
    std::lock_guard lock(writeMutex_);
    CallbackList newList = CloneList();
    for (auto& e : newList)
    {
        if (e.handle != handle) continue;
        e.enabled = enabled;
        StoreState(BuildState(std::move(newList)));
        return true;
    }
    return false;
}

bool ProcessEventHook::SetExecutionMode(CallbackHandle handle, ExecutionMode mode)
{
    DBG_ASSERT(handle > 0, "Invalid handle passed to SetExecutionMode");
    std::lock_guard lock(writeMutex_);
    CallbackList newList = CloneList();
    for (auto& e : newList)
    {
        if (e.handle != handle) continue;
        e.mode = mode;
        StoreState(BuildState(std::move(newList)));
        return true;
    }
    return false;
}

bool ProcessEventHook::SetExecutionTiming(CallbackHandle handle, ExecutionTiming timing)
{
    DBG_ASSERT(handle > 0, "Invalid handle passed to SetExecutionTiming");
    std::lock_guard lock(writeMutex_);
    CallbackList newList = CloneList();
    for (auto& e : newList)
    {
        if (e.handle != handle) continue;
        e.timing = timing;
        StoreState(BuildState(std::move(newList)));
        return true;
    }
    return false;
}

void ProcessEventHook::ClearAllCallbacks()
{
    std::lock_guard lock(writeMutex_);
    StoreState(std::make_shared<CallbackState>());
    DBG_ASSERT(std::atomic_load(&stateOwner_)->list.empty(), "ClearAllCallbacks failed");
}

size_t ProcessEventHook::GetCallbackCount() const
{
    auto s = std::atomic_load(&stateOwner_);
    if (s) { s->Validate(); return s->list.size(); }
    return 0;
}

// ── Free convenience functions ───────────────────────────────────────────────

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

CallbackHandle OnProcessEventByNameAndClass(const std::string& fn, UClass* cls, ProcessEventCallback cb, ClassMatchMode m, ExecutionTiming t, ExecutionMode mode)
{
    DBG_ASSERT(!fn.empty(), "Empty function name"); DBG_CHECK_PTR(cls); DBG_ASSERT(cb, "Null callback");
    return ProcessEventHook::Get().AddCallback(std::move(cb), fn, cls, nullptr, nullptr, m, t, mode);
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

CallbackHandle OnProcessEventByFunctionAndObject(UFunction* func, const UObject* obj, ProcessEventCallback cb, ExecutionTiming t, ExecutionMode mode)
{
    DBG_CHECK_PTR(func); DBG_CHECK_PTR(obj); DBG_ASSERT(cb, "Null callback");
    return ProcessEventHook::Get().AddCallback(std::move(cb), "", nullptr, obj, func, ClassMatchMode::ExactOrSubclass, t, mode);
}

CallbackHandle OnProcessEventAll(ProcessEventCallback cb, ExecutionTiming t, ExecutionMode mode)
{
    DBG_ASSERT(cb, "Null callback");
    return ProcessEventHook::Get().AddCallback(std::move(cb), "", nullptr, nullptr, nullptr, ClassMatchMode::ExactOrSubclass, t, mode);
}

CallbackHandle OnProcessEventAdvanced(ProcessEventCallback cb,
    const std::string& fn, UClass* cls, const UObject* obj,
    UFunction* func, ClassMatchMode m, ExecutionTiming t, ExecutionMode mode)
{
    DBG_ASSERT(cb, "Null callback");
    return ProcessEventHook::Get().AddCallback(std::move(cb), fn, cls, obj, func, m, t, mode);
}

CallbackHandle OnProcessEventAdvanced(const CallbackParams& p, ProcessEventCallback cb)
{
    DBG_ASSERT(cb, "Null callback");
    return ProcessEventHook::Get().AddCallback(std::move(cb),
        p.functionName, p.classFilter, p.objectFilter, p.functionFilter,
        p.classMatchMode, p.timing, p.mode);
}

bool RemoveHook           (CallbackHandle h) { DBG_ASSERT(h > 0, "Invalid handle"); return ProcessEventHook::Get().RemoveCallback(h); }
bool SetHookExecutionMode (CallbackHandle h, ExecutionMode m)   { DBG_ASSERT(h > 0, "Invalid handle"); return ProcessEventHook::Get().SetExecutionMode(h, m); }
bool SetHookExecutionTiming(CallbackHandle h, ExecutionTiming t) { DBG_ASSERT(h > 0, "Invalid handle"); return ProcessEventHook::Get().SetExecutionTiming(h, t); }

} // namespace GameHooks

void EnqueueOnce(std::function<void()> fn)
{
    GameHooks::ProcessEventHook::Get().Enqueue(std::move(fn));
}

void EnqueueWhile(std::function<bool()> fn)
{
    auto& hook = GameHooks::ProcessEventHook::Get();
    auto task = std::make_shared<std::function<void()>>();
    *task = [fn = std::move(fn), task, &hook]() mutable
        {
            if (fn()) hook.Enqueue(*task);
        };
    hook.Enqueue(*task);
}
