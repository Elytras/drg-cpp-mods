#pragma once
// Lib_GameHooks.h � ProcessEventHook and GameHooks convenience API.

#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <Windows.h>
#include "Lib_Forward.h"
#include "Lib_FField.h"
#include "Lib_Utils.h"
#include "Lib_EasyHook.h"
#include "Common.h"

#ifndef CatchFuncs
#define CatchFuncs 0
#endif

// Debug assertion macros
#ifdef _DEBUG
#define DBG_ASSERT(cond, msg) assert((cond) && msg)
#define DBG_CHECK_PTR(ptr) assert((ptr) != nullptr && "Null pointer encountered")
#define DBG_CHECK_RANGE(val, min, max) assert((val) >= (min) && (val) <= (max) && "Value out of range")
#else
#define DBG_ASSERT(cond, msg) ((void)0)
#define DBG_CHECK_PTR(ptr) ((void)0)
#define DBG_CHECK_RANGE(val, min, max) ((void)0)
#endif

extern ResponseBuffer* g_pRespBuffer;
extern HANDLE          g_hRespEvent;
extern void SendResponse(uint32_t cmdSeq, const std::string& msg);

namespace GameHooks
{
    enum class ExecutionTiming { Before, After, BeforeAndAfter };
    enum class ExecutionMode { CallOriginal, SkipOriginal };
    enum class ClassMatchMode { Exact, ExactOrSubclass, SubclassOnly };

    using ProcessEventCallback = std::function<void(UObject*, UFunction*, void*)>;
    using CallbackHandle = size_t;

    class ProcessEventHook
    {
    private:
        struct CallbackEntry
        {
            CallbackHandle       handle;
            std::string          functionNameFilter;
            FName                functionNameFName;
            UClass* classFilter;
            const UObject* objectFilter;
            UFunction* functionFilter;
            ClassMatchMode       classMatchMode;
            ExecutionTiming      timing;
            ExecutionMode        mode;
            ProcessEventCallback callback;
            bool                 enabled;

            CallbackEntry(CallbackHandle h, const std::string& funcName,
                UClass* cls, const UObject* obj, UFunction* func,
                ClassMatchMode clsMode, ExecutionTiming tim,
                ExecutionMode mod, ProcessEventCallback cb)
                : handle(h), functionNameFilter(funcName), functionNameFName{}
                , classFilter(cls), objectFilter(obj), functionFilter(func)
                , classMatchMode(clsMode), timing(tim), mode(mod)
                , callback(std::move(cb)), enabled(true) {
                DBG_ASSERT(h > 0, "Invalid callback handle");
                DBG_ASSERT(callback, "Callback function is null");
            }
        };

        struct DispatchCache
        {
            std::unordered_map<UFunction*, std::vector<size_t>> byFunctionPtr;
            std::unordered_map<FName, std::vector<size_t>> byFunctionName;
            std::vector<size_t>                                 wildcards;

            // Debug validation
            void Validate() const
            {
#ifdef _DEBUG
                size_t totalIndices = 0;
                for (const auto& pair : byFunctionPtr)
                {
                    DBG_CHECK_PTR(pair.first);
                    totalIndices += pair.second.size();
                }
                for (const auto& pair : byFunctionName)
                {
                    totalIndices += pair.second.size();
                }
                totalIndices += wildcards.size();
                // Indices should be reasonable (not negative or insane)
                DBG_CHECK_RANGE(totalIndices, 0, 100000);
#endif
            }
        };

        using CallbackList = std::vector<CallbackEntry>;
        struct CallbackState { 
            CallbackList list; 
            DispatchCache cache;

            void Validate() const
            {
#ifdef _DEBUG
                DBG_ASSERT(!list.empty() || cache.byFunctionPtr.empty(), "Non-empty cache with empty list");
                DBG_ASSERT(!list.empty() || cache.byFunctionName.empty(), "Non-empty cache with empty list");
                DBG_ASSERT(!list.empty() || cache.wildcards.empty(), "Non-empty cache with empty list");
                cache.Validate();
#endif
            }
        };

        static constexpr size_t               kMaxDispatchBuf = 32;

        static ProcessEventHook* instance;
        static void(*OriginalProcessEvent)(UObject*, UFunction*, void*);
        static inline std::atomic<int>        executionDepth_{ 0 };

        std::atomic<bool>                     pendingUninstall_{ false };
        std::atomic<bool>                     hookInstalled_{ false };
        std::atomic<bool>                     hasTasks_{ false };
        // atomic shared_ptr used to allow readers to obtain an owning snapshot via atomic_load.
        std::atomic<std::shared_ptr<const CallbackState>> stateOwner_{};
        mutable std::mutex                    writeMutex_;
        std::mutex                            taskMutex_;
        std::vector<std::function<void()>>    taskQueue_;
        std::function<void()>                 onUninstalled_;

        ProcessEventHook()
        {
            auto e = std::make_shared<CallbackState>();
            StoreState(e);
            DBG_ASSERT(stateOwner_.load() != nullptr, "Failed to initialize state");
        }

        void StoreState(std::shared_ptr<const CallbackState> s)
        {
            DBG_CHECK_PTR(s.get());
            s->Validate();
            std::atomic_store(&stateOwner_, std::move(s));
            // Verify store succeeded
            auto loaded = std::atomic_load(&stateOwner_);
            DBG_ASSERT(loaded != nullptr, "State store failed");
        }

        static std::shared_ptr<const CallbackState> BuildState(CallbackList list)
        {
            auto s = std::make_shared<CallbackState>();
            s->list = std::move(list);
            
            for (size_t i = 0; i < s->list.size(); ++i)
            {
                auto& e = s->list[i];
                DBG_ASSERT(e.handle > 0, "Invalid entry handle during state build");
                DBG_ASSERT(e.callback, "Null callback during state build");

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

        static bool MatchesClassFilter(UObject* Object, UClass* classFilter, ClassMatchMode mode)
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
            case ClassMatchMode::Exact:
                return cls == classFilter;
            case ClassMatchMode::ExactOrSubclass:
                return cls == classFilter || sc.IsSubclassOf(cls, classFilter);
            case ClassMatchMode::SubclassOnly:
                return cls != classFilter && sc.IsSubclassOf(cls, classFilter);
            default:
                DBG_ASSERT(false, "Unknown ClassMatchMode");
                return false;
            }
        }

        static bool RunBeforePass(const CallbackList& list, const size_t* idx, size_t count,
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
                DBG_ASSERT(e.callback, "Null callback in entry");

                if (!e.enabled || e.timing == ExecutionTiming::After) continue;
                if (e.objectFilter && e.objectFilter != Object)       continue;
                if (!MatchesClassFilter(Object, e.classFilter, e.classMatchMode)) continue;

                try
                {
                    e.callback(Object, Function, Parms);
                }
                catch (const std::exception& ex)
                {
                    DBG_ASSERT(false, "Callback threw exception");
                }

                if (e.mode == ExecutionMode::SkipOriginal) { ok = false; skipFired = true; }
                else                                         ++callOriginalRan;
            }

            if (skipFired && callOriginalRan > 0)
                spdlog::warn("[GameHooks] SkipOriginal suppressed the original for {} CallOriginal callback(s) on the same event (function: '{}')",
                    callOriginalRan,
                    Function && !Function->Name.IsNone() ? Function->Name.ToString() : "<unknown>");

            return ok;
        }

        static void RunAfterPass(const CallbackList& list, const size_t* idx, size_t count,
            UObject* Object, UFunction* Function, void* Parms)
        {
            DBG_CHECK_PTR(Object);
            DBG_CHECK_RANGE(count, 0, kMaxDispatchBuf);

            for (size_t n = 0; n < count; ++n)
            {
                DBG_CHECK_RANGE(idx[n], 0, list.size());
                const auto& e = list[idx[n]];

                DBG_ASSERT(e.handle > 0, "Invalid callback entry handle");
                DBG_ASSERT(e.callback, "Null callback in entry");

                if (!e.enabled || e.timing == ExecutionTiming::Before) continue;
                if (e.objectFilter && e.objectFilter != Object)        continue;
                if (!MatchesClassFilter(Object, e.classFilter, e.classMatchMode)) continue;

                try
                {
                    e.callback(Object, Function, Parms);
                }
                catch (const std::exception& ex)
                {
                    DBG_ASSERT(false, "Callback threw exception");
                }
            }
        }

        static void __fastcall HookedProcessEvent(UObject* Object, UFunction* Function, void* Parms)
        {
            if (!OriginalProcessEvent) return;

            DBG_CHECK_PTR(Object);
            // Function can be null in some cases, don't assert

            int depthBefore = executionDepth_.fetch_add(1, std::memory_order_relaxed);
            DBG_CHECK_RANGE(depthBefore, 0, 1000);  // Detect recursion depth issues

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

                size_t taskCount = 0;
                while (!local.empty()) 
                { 
                    DBG_ASSERT(local.back(), "Null task in queue");
                    try
                    {
                        local.back()(); 
                        taskCount++;
                    }
                    catch (const std::exception& ex)
                    {
                        DBG_ASSERT(false, "Task threw exception");
                    }
                    local.pop_back(); 
                }
                DBG_ASSERT(taskCount >= 0, "Task count invalid");
            }

            auto state = std::atomic_load(&inst->stateOwner_);
            if (state)
            {
                DBG_CHECK_PTR(state.get());
                state->Validate();

                if (!state->list.empty())
                {
                    size_t buf[kMaxDispatchBuf];
                    size_t cnt = 0;
                    auto push = [&](size_t i)
                    {
                        if (cnt < kMaxDispatchBuf)
                        {
                            DBG_CHECK_RANGE(i, 0, state->list.size());
                            buf[cnt++] = i; 
                        }
                        else
                        {
                            DBG_ASSERT(false, "Callback buffer overflow");
                        }
                    };

                    if (Function)
                    {
                        DBG_CHECK_PTR(Function);

                        if (auto it = state->cache.byFunctionPtr.find(Function); it != state->cache.byFunctionPtr.end())
                        {
                            for (size_t i : it->second) push(i);
                        }

                        if (auto it = state->cache.byFunctionName.find(Function->Name); it != state->cache.byFunctionName.end())
                        {
                            for (size_t i : it->second) push(i);
                        }
                    }

                    for (size_t i : state->cache.wildcards) push(i);

                    bool callOriginal = RunBeforePass(state->list, buf, cnt, Object, Function, Parms);

                    if (callOriginal)
                    {
                        __assume(OriginalProcessEvent != nullptr);
                        try
                        {
                            OriginalProcessEvent(Object, Function, Parms);
                        }
                        catch (const std::exception& ex)
                        {
                            DBG_ASSERT(false, "OriginalProcessEvent threw exception");
                        }
                    }

                    RunAfterPass(state->list, buf, cnt, Object, Function, Parms);
                }
            }
            else
            {
                __assume(OriginalProcessEvent != nullptr);
                try
                {
                    OriginalProcessEvent(Object, Function, Parms);
                }
                catch (const std::exception& ex)
                {
                    DBG_ASSERT(false, "OriginalProcessEvent threw exception");
                }
            }

            int depthAfter = executionDepth_.fetch_sub(1, std::memory_order_acq_rel);
            DBG_CHECK_RANGE(depthAfter, 1, 1000);  // Should be at least 1 before decrement
            DBG_ASSERT(depthAfter > 0, "Execution depth went negative");

            if (depthAfter == 1 && inst->pendingUninstall_.load(std::memory_order_relaxed))
            {
                bool expected = true;
                if (inst->pendingUninstall_.compare_exchange_strong(expected, false))
                {
                    try
                    {
                        inst->DoUninstall();
                    }
                    catch (const std::exception& ex)
                    {
                        DBG_ASSERT(false, "DoUninstall threw exception");
                    }
                }
            }
        }

        bool DoUninstall()
        {
            DBG_ASSERT(!hookInstalled_.load() || pendingUninstall_.load(), "Uninstall without pending flag");

            { 
                std::lock_guard lock(writeMutex_); 
                StoreState(std::make_shared<CallbackState>()); 
            }

            int execDepth = executionDepth_.load(std::memory_order_relaxed);
            DBG_ASSERT(execDepth == 0, "DoUninstall called with non-zero execution depth");

            EasyHook::Shutdown();
            hookInstalled_.store(false);
            DBG_ASSERT(!hookInstalled_.load(), "Failed to clear hookInstalled flag");

            if (onUninstalled_) 
            { 
                auto cb = std::move(onUninstalled_); 
                onUninstalled_ = nullptr; 
                try
                {
                    cb(); 
                }
                catch (const std::exception& ex)
                {
                    DBG_ASSERT(false, "onUninstalled callback threw exception");
                }
            }
            return true;
        }

        CallbackList CloneList() const 
        { 
            auto s = std::atomic_load(&stateOwner_); 
            if (s)
            {
                s->Validate();
                return s->list;
            }
            return CallbackList{}; 
        }

    public:
        static ProcessEventHook& Get() 
        { 
            if (!instance) instance = new ProcessEventHook();
            DBG_CHECK_PTR(instance);
            return *instance; 
        }

        bool IsInstalled() const 
        { 
            return hookInstalled_.load(); 
        }

        bool Install()
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
            DBG_ASSERT(hookInstalled_.load(), "Failed to set hookInstalled flag");
            return true;
        }

        bool RequestUninstall()
        {
            int depth = executionDepth_.load();
            DBG_CHECK_RANGE(depth, 0, 1000);

            if (depth > 0) 
            { 
                pendingUninstall_ = true; 
                DBG_ASSERT(pendingUninstall_.load(), "Failed to set pendingUninstall flag");
                return true; 
            }
            return DoUninstall();
        }

        void SetOnUninstalled(std::function<void()> cb) 
        { 
            DBG_ASSERT(cb, "Null callback passed to SetOnUninstalled");
            onUninstalled_ = std::move(cb); 
        }

        void Enqueue(std::function<void()> task)
        {
            DBG_ASSERT(task, "Null task passed to Enqueue");

            std::lock_guard lock(taskMutex_);
            taskQueue_.push_back(std::move(task));
            DBG_CHECK_RANGE(taskQueue_.size(), 1, 100000);
            hasTasks_.store(true, std::memory_order_release);
            DBG_ASSERT(hasTasks_.load(), "Failed to set hasTasks flag");
        }

        CallbackHandle AddCallback(ProcessEventCallback callback,
            const std::string& functionName = "", UClass* classFilter = nullptr,
            const UObject* objectFilter = nullptr, UFunction* functionFilter = nullptr,
            ClassMatchMode classMatchMode = ClassMatchMode::ExactOrSubclass,
            ExecutionTiming timing = ExecutionTiming::Before,
            ExecutionMode mode = ExecutionMode::CallOriginal)
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

        bool RemoveCallback(CallbackHandle handle)
        {
            DBG_ASSERT(handle > 0, "Invalid handle passed to RemoveCallback");

            std::lock_guard lock(writeMutex_);
            CallbackList newList = CloneList();
            auto it = std::remove_if(newList.begin(), newList.end(),
                [handle](const CallbackEntry& e) { return e.handle == handle; });
            if (it == newList.end()) return false;
            newList.erase(it, newList.end());
            DBG_CHECK_RANGE(newList.size(), 0, 100000);
            StoreState(BuildState(std::move(newList)));
            return true;
        }

        bool EnableCallback(CallbackHandle handle, bool enabled)
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

        bool SetExecutionMode(CallbackHandle handle, ExecutionMode mode)
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

        bool SetExecutionTiming(CallbackHandle handle, ExecutionTiming timing)
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

        void   ClearAllCallbacks() 
        { 
            std::lock_guard lock(writeMutex_); 
            StoreState(std::make_shared<CallbackState>()); 
            auto s = std::atomic_load(&stateOwner_);
            DBG_ASSERT(s && s->list.empty(), "ClearAllCallbacks failed");
        }

        size_t GetCallbackCount()  const 
        { 
            auto s = std::atomic_load(&stateOwner_); 
            if (s)
            {
                s->Validate();
                return s->list.size();
            }
            return 0; 
        }

    private:
        CallbackHandle nextHandle_{ 1 };
    };

    inline ProcessEventHook* ProcessEventHook::instance = nullptr;
    inline void(*ProcessEventHook::OriginalProcessEvent)(UObject*, UFunction*, void*) = nullptr;

    inline bool InstallProcessEventHook() 
    { 
        return ProcessEventHook::Get().Install(); 
    }

    inline CallbackHandle OnProcessEventByName(const std::string& fn, ProcessEventCallback cb)
    {
        DBG_ASSERT(!fn.empty(), "Empty function name");
        DBG_ASSERT(cb, "Null callback");
        return ProcessEventHook::Get().AddCallback(std::move(cb), fn);
    }

    inline CallbackHandle OnProcessEventByClass(UClass* cls, ProcessEventCallback cb, ClassMatchMode m = ClassMatchMode::ExactOrSubclass)
    {
        DBG_CHECK_PTR(cls);
        DBG_ASSERT(cb, "Null callback");
        return ProcessEventHook::Get().AddCallback(std::move(cb), "", cls, nullptr, nullptr, m);
    }

    inline CallbackHandle OnProcessEventByNameAndClass(const std::string& fn, UClass* cls, ProcessEventCallback cb, ClassMatchMode m = ClassMatchMode::ExactOrSubclass)
    {
        DBG_ASSERT(!fn.empty(), "Empty function name");
        DBG_CHECK_PTR(cls);
        DBG_ASSERT(cb, "Null callback");
        return ProcessEventHook::Get().AddCallback(std::move(cb), fn, cls, nullptr, nullptr, m);
    }

    inline CallbackHandle OnProcessEventByObject(const UObject* obj, ProcessEventCallback cb)
    {
        DBG_CHECK_PTR(obj);
        DBG_ASSERT(cb, "Null callback");
        return ProcessEventHook::Get().AddCallback(std::move(cb), "", nullptr, obj);
    }

    inline CallbackHandle OnProcessEventByFunction(UFunction* func, ProcessEventCallback cb)
    {
        DBG_CHECK_PTR(func);
        DBG_ASSERT(cb, "Null callback");
        return ProcessEventHook::Get().AddCallback(std::move(cb), "", nullptr, nullptr, func);
    }

    inline CallbackHandle OnProcessEventByFunctionAndObject(UFunction* func, const UObject* obj, ProcessEventCallback cb)
    {
        DBG_CHECK_PTR(func);
        DBG_CHECK_PTR(obj);
        DBG_ASSERT(cb, "Null callback");
        return ProcessEventHook::Get().AddCallback(std::move(cb), "", nullptr, obj, func);
    }

    inline CallbackHandle OnProcessEventAll(ProcessEventCallback cb)
    {
        DBG_ASSERT(cb, "Null callback");
        return ProcessEventHook::Get().AddCallback(std::move(cb));
    }

    inline CallbackHandle OnProcessEventAdvanced(ProcessEventCallback cb,
        const std::string& fn = "", UClass* cls = nullptr, const UObject* obj = nullptr,
        UFunction* func = nullptr, ClassMatchMode m = ClassMatchMode::ExactOrSubclass,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal)
    {
        DBG_ASSERT(cb, "Null callback");
        return ProcessEventHook::Get().AddCallback(std::move(cb), fn, cls, obj, func, m, t, mode);
    }

    inline bool RemoveHook(CallbackHandle h) 
    { 
        DBG_ASSERT(h > 0, "Invalid handle");
        return ProcessEventHook::Get().RemoveCallback(h); 
    }

    inline bool SetHookExecutionMode(CallbackHandle h, ExecutionMode m) 
    { 
        DBG_ASSERT(h > 0, "Invalid handle");
        return ProcessEventHook::Get().SetExecutionMode(h, m); 
    }

    inline bool SetHookExecutionTiming(CallbackHandle h, ExecutionTiming t) 
    { 
        DBG_ASSERT(h > 0, "Invalid handle");
        return ProcessEventHook::Get().SetExecutionTiming(h, t); 
    }

} // namespace GameHooks

// Enqueues work items one at a time on the game thread, rate-limited by interval.
// fn receives the current item and its index. Returns early if fn returns false.
template<typename T, bool CanThrow = true>
void EnqueueThrottled(
    std::vector<T> items,
    std::chrono::milliseconds interval,
    std::function<bool(T&, size_t, size_t)> fn)
{
    auto& hook = GameHooks::ProcessEventHook::Get();

    auto shared = std::make_shared<std::vector<T>>(std::move(items));
    auto index = std::make_shared<size_t>(0);
    auto lastFire = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now() - interval);

    auto schedule = std::make_shared<std::function<void()>>();

    *schedule = [shared, index, lastFire, interval, fn, schedule, &hook]() mutable
        {
            if (*index >= shared->size())
                return;

            auto now = std::chrono::steady_clock::now();
            if (now - *lastFire < interval)
            {
                hook.Enqueue(*schedule);
                return;
            }

            *lastFire = now;

            T& item = (*shared)[*index];

            bool keepGoing = false;
            if constexpr (CanThrow) {
                try
                {
                    keepGoing = fn(item, *index, shared->size());
                }
                catch (...)
                {
                    keepGoing = false;
                }
            }
            else {
                keepGoing = fn(item, *index, shared->size());
            }

            ++(*index);

            if (keepGoing && *index < shared->size())
                hook.Enqueue(*schedule);
        };

    // IMPORTANT: start execution immediately, not delayed
    hook.Enqueue(*schedule);
}