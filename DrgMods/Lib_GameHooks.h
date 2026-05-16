#pragma once
// Lib_GameHooks.h — ProcessEventHook and GameHooks convenience API.

#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <type_traits>
#include <Windows.h>
#include "Lib_Forward.h"
#include "Lib_Utils.h"
#include "Lib_EasyHook.h"
#include "Common.h"

#ifndef CatchFuncs
#define CatchFuncs 0
#endif

#ifdef _DEBUG
#define DBG_ASSERT(cond, msg)            assert((cond) && msg)
#define DBG_CHECK_PTR(ptr)               assert((ptr) != nullptr && "Null pointer encountered")
#define DBG_CHECK_RANGE(val, min, max)   assert((val) >= (min) && (val) <= (max) && "Value out of range")
#else
#define DBG_ASSERT(cond, msg)            ((void)0)
#define DBG_CHECK_PTR(ptr)               ((void)0)
#define DBG_CHECK_RANGE(val, min, max)   ((void)0)
#endif

extern ResponseBuffer* g_pRespBuffer;
extern HANDLE          g_hRespEvent;
extern void SendResponse(uint32_t cmdSeq, const std::string& msg);

namespace GameHooks
{
    enum class ExecutionTiming  { Before, After, BeforeAndAfter };
    enum class ExecutionMode    { CallOriginal, SkipOriginal };
    enum class ClassMatchMode   { Exact, ExactOrSubclass, SubclassOnly };

    using ProcessEventCallback = std::function<void(UObject*, UFunction*, void*)>;
    using CallbackHandle       = size_t;

    class ProcessEventHook
    {
    private:
        // ── Nested types ────────────────────────────────────────────────────

        struct CallbackEntry
        {
            CallbackHandle       handle;
            std::string          functionNameFilter;
            FName                functionNameFName;
            UClass*              classFilter;
            const UObject*       objectFilter;
            UFunction*           functionFilter;
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
                , callback(std::move(cb)), enabled(true)
            {
                DBG_ASSERT(h > 0,     "Invalid callback handle");
                DBG_ASSERT(callback,  "Callback function is null");
            }
        };

        struct DispatchCache
        {
            std::unordered_map<UFunction*, std::vector<size_t>> byFunctionPtr;
            std::unordered_map<FName,      std::vector<size_t>> byFunctionName;
            std::vector<size_t>                                 wildcards;

            void Validate() const
            {
#ifdef _DEBUG
                size_t total = 0;
                for (const auto& p : byFunctionPtr)  { DBG_CHECK_PTR(p.first); total += p.second.size(); }
                for (const auto& p : byFunctionName)  total += p.second.size();
                total += wildcards.size();
                DBG_CHECK_RANGE(total, 0, 100000);
#endif
            }
        };

        using CallbackList = std::vector<CallbackEntry>;

        struct CallbackState
        {
            CallbackList  list;
            DispatchCache cache;

            void Validate() const
            {
#ifdef _DEBUG
                DBG_ASSERT(!list.empty() || cache.byFunctionPtr.empty(),  "Non-empty cache with empty list");
                DBG_ASSERT(!list.empty() || cache.byFunctionName.empty(), "Non-empty cache with empty list");
                DBG_ASSERT(!list.empty() || cache.wildcards.empty(),      "Non-empty cache with empty list");
                cache.Validate();
#endif
            }
        };

        // ── Static / member data ─────────────────────────────────────────────

        static constexpr size_t          kMaxDispatchBuf = 32;

        static ProcessEventHook*         instance;
        static void(*OriginalProcessEvent)(UObject*, UFunction*, void*);
        static inline std::atomic<int>   executionDepth_{ 0 };

        std::atomic<bool>                              pendingUninstall_{ false };
        std::atomic<bool>                              hookInstalled_{ false };
        std::atomic<bool>                              hasTasks_{ false };
        std::atomic<std::shared_ptr<const CallbackState>> stateOwner_{};
        mutable std::mutex                             writeMutex_;
        std::mutex                                     taskMutex_;
        std::vector<std::function<void()>>             taskQueue_;
        std::function<void()>                          onUninstalled_;
        CallbackHandle                                 nextHandle_{ 1 };

        // ── Private methods ──────────────────────────────────────────────────

        ProcessEventHook();
        void StoreState(std::shared_ptr<const CallbackState> s);
        static std::shared_ptr<const CallbackState> BuildState(CallbackList list);
        static bool MatchesClassFilter(UObject* Object, UClass* classFilter, ClassMatchMode mode);
        static bool RunBeforePass(const CallbackList& list, const size_t* idx, size_t count,
                                  UObject* Object, UFunction* Function, void* Parms);
        static void RunAfterPass (const CallbackList& list, const size_t* idx, size_t count,
                                  UObject* Object, UFunction* Function, void* Parms);
        static void __fastcall HookedProcessEvent(UObject* Object, UFunction* Function, void* Parms);
        bool DoUninstall();
        CallbackList CloneList() const;

    public:
        static ProcessEventHook& Get();

        bool IsInstalled() const;
        bool Install();
        bool RequestUninstall();
        void SetOnUninstalled(std::function<void()> cb);
        void Enqueue(std::function<void()> task);

        CallbackHandle AddCallback(ProcessEventCallback callback,
            const std::string& functionName = "", UClass* classFilter = nullptr,
            const UObject* objectFilter = nullptr, UFunction* functionFilter = nullptr,
            ClassMatchMode classMatchMode = ClassMatchMode::ExactOrSubclass,
            ExecutionTiming timing = ExecutionTiming::Before,
            ExecutionMode mode = ExecutionMode::CallOriginal);

        bool RemoveCallback    (CallbackHandle handle);
        bool EnableCallback    (CallbackHandle handle, bool enabled);
        bool SetExecutionMode  (CallbackHandle handle, ExecutionMode mode);
        bool SetExecutionTiming(CallbackHandle handle, ExecutionTiming timing);
        void   ClearAllCallbacks();
        size_t GetCallbackCount() const;
    };

    // ── Free convenience functions ────────────────────────────────────────────

    bool InstallProcessEventHook();

    CallbackHandle OnProcessEventByName(const std::string& fn, ProcessEventCallback cb,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    CallbackHandle OnProcessEventByClass(UClass* cls, ProcessEventCallback cb,
        ClassMatchMode m = ClassMatchMode::ExactOrSubclass,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    CallbackHandle OnProcessEventByNameAndClass(const std::string& fn, UClass* cls, ProcessEventCallback cb,
        ClassMatchMode m = ClassMatchMode::ExactOrSubclass,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    CallbackHandle OnProcessEventByObject(const UObject* obj, ProcessEventCallback cb,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    CallbackHandle OnProcessEventByFunction(UFunction* func, ProcessEventCallback cb,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    CallbackHandle OnProcessEventByFunctionAndObject(UFunction* func, const UObject* obj, ProcessEventCallback cb,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    CallbackHandle OnProcessEventAll(ProcessEventCallback cb,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    struct CallbackParams
    {
        std::string     functionName   = "";
        UClass*         classFilter    = nullptr;
        const UObject*  objectFilter   = nullptr;
        UFunction*      functionFilter = nullptr;
        ClassMatchMode  classMatchMode = ClassMatchMode::ExactOrSubclass;
        ExecutionTiming timing         = ExecutionTiming::Before;
        ExecutionMode   mode           = ExecutionMode::CallOriginal;
    };

    CallbackHandle OnProcessEventAdvanced(ProcessEventCallback cb,
        const std::string& fn = "", UClass* cls = nullptr, const UObject* obj = nullptr,
        UFunction* func = nullptr, ClassMatchMode m = ClassMatchMode::ExactOrSubclass,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    CallbackHandle OnProcessEventAdvanced(const CallbackParams& p, ProcessEventCallback cb);

    bool RemoveHook           (CallbackHandle h);
    bool SetHookExecutionMode (CallbackHandle h, ExecutionMode m);
    bool SetHookExecutionTiming(CallbackHandle h, ExecutionTiming t);

} // namespace GameHooks

// =========================================================================
// EnqueueThrottled — schedules items one at a time on the game thread,
//   rate-limited by interval. fn(item, index, total) returns false to stop.
// =========================================================================

template<typename T, bool CanThrow = true>
void EnqueueThrottled(
    std::vector<T> items,
    std::chrono::milliseconds interval,
    std::function<bool(T&, size_t, size_t)> fn)
{
    auto& hook = GameHooks::ProcessEventHook::Get();

    auto shared   = std::make_shared<std::vector<T>>(std::move(items));
    auto index    = std::make_shared<size_t>(0);
    auto lastFire = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now() - interval);
    auto schedule = std::make_shared<std::function<void()>>();

    *schedule = [shared, index, lastFire, interval, fn, schedule, &hook]() mutable
    {
        if (*index >= shared->size()) return;

        auto now = std::chrono::steady_clock::now();
        if (now - *lastFire < interval) { hook.Enqueue(*schedule); return; }

        *lastFire = now;
        T& item = (*shared)[*index];

        bool keepGoing = false;
        if constexpr (CanThrow) {
            try   { keepGoing = fn(item, *index, shared->size()); }
            catch (...) { keepGoing = false; }
        } else {
            keepGoing = fn(item, *index, shared->size());
        }

        ++(*index);
        if (keepGoing && *index < shared->size()) hook.Enqueue(*schedule);
    };

    hook.Enqueue(*schedule);
}

// =========================================================================
// EnqueueDelayed — executes fn once on the game thread after a delay.
//   EnqueueDelayed(fn)              — 200 ms  (default)
//   EnqueueDelayed(fn, 500)         — 500 ms
//   EnqueueDelayed<true>(fn, 1.5f)  — 1.5 s
// =========================================================================

template<typename T>
concept DelayNumeric = std::is_arithmetic_v<T>
                    && !std::is_same_v<T, bool>
                    && !std::is_same_v<T, char>
                    && !std::is_same_v<T, wchar_t>;

template<bool IsSeconds = false, DelayNumeric T = int>
void EnqueueDelayed(std::function<void()> fn, T delay = T(200))
{
    delay = delay >= T(0) ? delay : T(0);

    using namespace std::chrono;
    auto ms = IsSeconds
        ? duration_cast<milliseconds>(duration<long double>(delay))
        : milliseconds(static_cast<long long>(delay));

    auto& hook  = GameHooks::ProcessEventHook::Get();
    auto fireAt = std::make_shared<steady_clock::time_point>(steady_clock::now() + ms);
    auto task   = std::make_shared<std::function<void()>>();

    *task = [fireAt, fn = std::move(fn), task, &hook]() mutable
    {
        if (steady_clock::now() < *fireAt) { hook.Enqueue(*task); return; }
        fn();
    };

    hook.Enqueue(*task);
}
