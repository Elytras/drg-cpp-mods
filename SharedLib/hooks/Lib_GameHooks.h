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

// Defined in Lib_ObjectFactory.h (pulled in before this header via Library.h).
// Forward-declared here so the SCO hook's callback signature compiles without
// making this widely-included header depend on ObjectFactory.
struct FStaticConstructObjectParameters;

namespace GameHooks
{
    enum class ExecutionTiming  { Before, After, BeforeAndAfter };
    enum class ExecutionMode    { CallOriginal, SkipOriginal };
    enum class ClassMatchMode   { Exact, ExactOrSubclass, SubclassOnly };

    using ProcessEventCallback = std::function<void(SDK::UObject*, SDK::UFunction*, void*)>;
    using CallbackHandle       = size_t;

    // =========================================================================
    // HookBase<Derived> — CRTP base for hook singletons.
    // Provides: singleton, lifecycle, task queue, and callback CRUD.
    //
    // Derived must implement (kept private, accessed via friend):
    //   auto  CloneList() const          — copy of the current callback list
    //   void  StoreList(CallbackList)    — commit a new list (may rebuild dispatch cache)
    //   void  StoreEmptyState()          — fast path: store an empty state
    //   bool  DoUninstall()              — hook-specific teardown; must call DoUninstallCommon()
    // =========================================================================

    template<typename Derived>
    class HookBase
    {
    protected:
        static inline std::atomic<int>     executionDepth_{ 0 };

        std::atomic<bool>                  pendingUninstall_{ false };
        std::atomic<bool>                  hookInstalled_{ false };
        std::atomic<bool>                  hasTasks_{ false };
        // Hot-path gate: mirrors "the committed state has >= 1 callback". Set by
        // each Derived::StoreState (release) right after the state is published;
        // read (acquire) at the top of the Hooked* trampolines so the common
        // zero-callback case skips the atomic<shared_ptr> load entirely. A stale
        // read only costs one extra slow-path load (true) or one missed dispatch
        // during a concurrent (un)register (false) — both benign and transient.
        std::atomic<bool>                  hasCallbacks_{ false };
        mutable std::mutex                 writeMutex_;
        std::mutex                         taskMutex_;
        std::vector<std::function<void()>> taskQueue_;
        std::function<void()>              onUninstalled_;
        CallbackHandle                     nextHandle_{ 1 };
        void*                              hookAddr_{ nullptr };  // MinHook target; set by Derived::Install, removed on uninstall

        HookBase() = default;

        // Drain the task queue — call from the hooked function when depthBefore == 0.
        void DrainTasks()
        {
            if (!hasTasks_.load(std::memory_order_acquire)) return;
            std::vector<std::function<void()>> local;
            {
                std::lock_guard lock(taskMutex_);
                std::swap(local, taskQueue_);
                hasTasks_.store(false, std::memory_order_relaxed);
            }
            for (auto& t : local)
            {
                DBG_ASSERT(t, "Null task in queue");
                try { t(); }
                catch (const std::exception&) { DBG_ASSERT(false, "Task threw exception"); }
            }
        }

        bool DoUninstallCommon()
        {
            DBG_ASSERT(!hookInstalled_.load() || pendingUninstall_.load(), "Uninstall without pending flag");
            {
                std::lock_guard lock(writeMutex_);
                static_cast<Derived*>(this)->StoreEmptyState();
            }
            DBG_ASSERT(executionDepth_.load(std::memory_order_relaxed) == 0,
                "DoUninstall called with non-zero execution depth");
            // Reference-counted teardown: each hook removes only ITS OWN MinHook
            // target. EasyHook releases MinHook once the last hook is gone, so no
            // hook "owns" global teardown. The executionDepth_ assert above
            // guarantees no thread is inside this hook's trampoline right now.
            if (hookAddr_)
            {
                EasyHook::HookManager::Get().RemoveHook(hookAddr_);
                hookAddr_ = nullptr;
            }
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

    public:
        static Derived& Get()
        {
            static Derived* inst = new Derived();
            DBG_CHECK_PTR(inst);
            return *inst;
        }

        bool IsInstalled() const { return hookInstalled_.load(); }

        bool RequestUninstall()
        {
            int depth = executionDepth_.load();
            DBG_CHECK_RANGE(depth, 0, 1000);
            if (depth > 0) { pendingUninstall_ = true; return true; }
            return static_cast<Derived*>(this)->DoUninstall();
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
        }

        bool RemoveCallback(CallbackHandle handle)
        {
            DBG_ASSERT(handle > 0, "Invalid handle passed to RemoveCallback");
            std::lock_guard lock(writeMutex_);
            auto list = static_cast<Derived*>(this)->CloneList();
            auto it = std::remove_if(list.begin(), list.end(),
                [handle](const auto& e) { return e.handle == handle; });
            if (it == list.end()) return false;
            list.erase(it, list.end());
            static_cast<Derived*>(this)->StoreList(std::move(list));
            return true;
        }

        bool EnableCallback(CallbackHandle handle, bool enabled)
        {
            DBG_ASSERT(handle > 0, "Invalid handle passed to EnableCallback");
            std::lock_guard lock(writeMutex_);
            auto list = static_cast<Derived*>(this)->CloneList();
            for (auto& e : list)
            {
                if (e.handle != handle) continue;
                e.enabled = enabled;
                static_cast<Derived*>(this)->StoreList(std::move(list));
                return true;
            }
            return false;
        }

        bool SetExecutionMode(CallbackHandle handle, ExecutionMode mode)
        {
            DBG_ASSERT(handle > 0, "Invalid handle passed to SetExecutionMode");
            std::lock_guard lock(writeMutex_);
            auto list = static_cast<Derived*>(this)->CloneList();
            for (auto& e : list)
            {
                if (e.handle != handle) continue;
                e.mode = mode;
                static_cast<Derived*>(this)->StoreList(std::move(list));
                return true;
            }
            return false;
        }

        bool SetExecutionTiming(CallbackHandle handle, ExecutionTiming timing)
        {
            DBG_ASSERT(handle > 0, "Invalid handle passed to SetExecutionTiming");
            std::lock_guard lock(writeMutex_);
            auto list = static_cast<Derived*>(this)->CloneList();
            for (auto& e : list)
            {
                if (e.handle != handle) continue;
                e.timing = timing;
                static_cast<Derived*>(this)->StoreList(std::move(list));
                return true;
            }
            return false;
        }

        void ClearAllCallbacks()
        {
            std::lock_guard lock(writeMutex_);
            static_cast<Derived*>(this)->StoreEmptyState();
        }

        size_t GetCallbackCount() const
        {
            return static_cast<const Derived*>(this)->CloneList().size();
        }
    };

    // =========================================================================
    // ProcessEventHook
    // =========================================================================

    class ProcessEventHook : public HookBase<ProcessEventHook>
    {
        friend class HookBase<ProcessEventHook>;

    private:
        // ── Nested types ────────────────────────────────────────────────────

        struct CallbackEntry
        {
            CallbackHandle       handle;
            std::string          functionNameFilter;
            SDK::FName           functionNameFName;
            SDK::UClass*         classFilter;
            const SDK::UObject*  objectFilter;
            SDK::UFunction*      functionFilter;
            ClassMatchMode       classMatchMode;
            ExecutionTiming      timing;
            ExecutionMode        mode;
            ProcessEventCallback callback;
            bool                 enabled;

            CallbackEntry(CallbackHandle h, const std::string& funcName,
                SDK::UClass* cls, const SDK::UObject* obj, SDK::UFunction* func,
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
            std::unordered_map<SDK::UFunction*, std::vector<size_t>> byFunctionPtr;
            std::unordered_map<SDK::FName,      std::vector<size_t>> byFunctionName;
            std::vector<size_t>                                      wildcards;

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

        // ── Data ────────────────────────────────────────────────────────────

        static constexpr size_t          kMaxDispatchBuf = 32;
        static void(*OriginalProcessEvent)(SDK::UObject*, SDK::UFunction*, void*);

        std::atomic<std::shared_ptr<const CallbackState>> stateOwner_{};

        // ── Internal helpers ─────────────────────────────────────────────────

        ProcessEventHook();
        void StoreState(std::shared_ptr<const CallbackState> s);
        static std::shared_ptr<const CallbackState> BuildState(CallbackList list);
        static bool MatchesClassFilter(SDK::UObject* Object, SDK::UClass* classFilter, ClassMatchMode mode);
        static bool RunBeforePass(const CallbackList& list, const size_t* idx, size_t count,
                                  SDK::UObject* Object, SDK::UFunction* Function, void* Parms);
        static void RunAfterPass (const CallbackList& list, const size_t* idx, size_t count,
                                  SDK::UObject* Object, SDK::UFunction* Function, void* Parms);
        static void __fastcall HookedProcessEvent(SDK::UObject* Object, SDK::UFunction* Function, void* Parms);

        // ── CRTP interface (called by HookBase) ──────────────────────────────

        CallbackList CloneList() const;
        void StoreList(CallbackList list);  // rebuilds dispatch cache then commits
        void StoreEmptyState();
        bool DoUninstall();

    public:
        // Snapshot of dispatch-cache occupancy — for diagnostics (dumpdispatch).
        // Wildcards run on EVERY ProcessEvent, so a high count defeats the cache.
        struct DispatchStats
        {
            size_t totalCallbacks     = 0;
            size_t enabled            = 0;
            size_t byFunctionPtrKeys  = 0;  // distinct UFunction* buckets
            size_t byFunctionNameKeys = 0;  // distinct FName buckets
            size_t wildcards          = 0;  // callbacks with no name/func filter
        };
        DispatchStats GetDispatchStats() const;

        bool Install();

        CallbackHandle AddCallback(ProcessEventCallback callback,
            const std::string& functionName = "", SDK::UClass* classFilter = nullptr,
            const SDK::UObject* objectFilter = nullptr, SDK::UFunction* functionFilter = nullptr,
            ClassMatchMode classMatchMode = ClassMatchMode::ExactOrSubclass,
            ExecutionTiming timing = ExecutionTiming::Before,
            ExecutionMode mode = ExecutionMode::CallOriginal);
    };

    // ── Free convenience functions ────────────────────────────────────────────

    bool InstallProcessEventHook();

    CallbackHandle OnProcessEventByName(const std::string& fn, ProcessEventCallback cb,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    CallbackHandle OnProcessEventByClass(SDK::UClass* cls, ProcessEventCallback cb,
        ClassMatchMode m = ClassMatchMode::ExactOrSubclass,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    // NOTE: multi-filter combinations (name+class, function+object, etc.) live on
    // the OnProcessEvent() builder below — the fixed-arity ByXAndY helpers were
    // retired because the arg order got ambiguous and didn't compose.
    CallbackHandle OnProcessEventByObject(const SDK::UObject* obj, ProcessEventCallback cb,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    CallbackHandle OnProcessEventByFunction(SDK::UFunction* func, ProcessEventCallback cb,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    CallbackHandle OnProcessEventAll(ProcessEventCallback cb,
        ExecutionTiming t = ExecutionTiming::Before, ExecutionMode mode = ExecutionMode::CallOriginal);

    struct CallbackParams
    {
        std::string         functionName   = "";
        SDK::UClass*        classFilter    = nullptr;
        const SDK::UObject* objectFilter   = nullptr;
        SDK::UFunction*     functionFilter = nullptr;
        ClassMatchMode      classMatchMode = ClassMatchMode::ExactOrSubclass;
        ExecutionTiming     timing         = ExecutionTiming::Before;
        ExecutionMode       mode           = ExecutionMode::CallOriginal;
    };

    // =========================================================================
    // ProcessEventBuilder — fluent, collision-proof way to register a PE callback.
    //
    // The fixed-arity OnProcessEventByX / OnProcessEventByXAndY helpers don't
    // compose: every new filter combination needs another named overload, and the
    // arg order (name-then-class vs class-then-name) gets ambiguous. The builder
    // is the single canonical entry point — chain only the filters you need, then
    // Bind(). It populates a CallbackParams and registers it via ProcessEventHook::AddCallback.
    //
    //   GameHooks::OnProcessEvent()
    //       .Class(AFoo::StaticClass()).Name("ReceiveTick")
    //       .Timing(ExecutionTiming::After)
    //       .Bind([](UObject* o, UFunction* f, void* p){ ... });
    //
    // Filters left unset match everything (same semantics as CallbackParams
    // defaults). The terminal Bind() returns the CallbackHandle.
    // =========================================================================
    class ProcessEventBuilder
    {
        CallbackParams p_;
    public:
        ProcessEventBuilder& Name    (std::string fn)         { p_.functionName   = std::move(fn); return *this; }
        ProcessEventBuilder& Class   (SDK::UClass* cls)       { p_.classFilter    = cls;           return *this; }
        ProcessEventBuilder& Object  (const SDK::UObject* o)  { p_.objectFilter   = o;             return *this; }
        ProcessEventBuilder& Function(SDK::UFunction* fn)     { p_.functionFilter = fn;            return *this; }
        ProcessEventBuilder& Match   (ClassMatchMode m)       { p_.classMatchMode = m;             return *this; }
        ProcessEventBuilder& Timing  (ExecutionTiming t)      { p_.timing         = t;             return *this; }
        ProcessEventBuilder& Mode    (ExecutionMode m)        { p_.mode           = m;             return *this; }

        // Convenience: SkipOriginal is the common non-default mode.
        ProcessEventBuilder& Skip()                           { p_.mode = ExecutionMode::SkipOriginal; return *this; }

        // Terminal — registers the callback, returns its handle.
        CallbackHandle Bind(ProcessEventCallback cb) const
        {
            return ProcessEventHook::Get().AddCallback(std::move(cb),
                p_.functionName, p_.classFilter, p_.objectFilter, p_.functionFilter,
                p_.classMatchMode, p_.timing, p_.mode);
        }
    };

    inline ProcessEventBuilder OnProcessEvent() { return {}; }

    bool RemoveHook            (CallbackHandle h);
    bool SetHookExecutionMode  (CallbackHandle h, ExecutionMode m);
    bool SetHookExecutionTiming(CallbackHandle h, ExecutionTiming t);

    // =========================================================================
    // EngineTickHook — generic hook on UGameEngine::Tick.
    //
    //   Signature on the wire: void(UGameEngine* this, float DeltaSeconds, bool bIdleMode)
    //
    // Unlike ProcessEventHook there are no per-callback filters — Tick is a
    // single virtual on the engine, so every registered callback fires every
    // frame at the requested timing.
    //
    // The vtable slot of UEngine::Tick is per-game and must be supplied to
    // Install() — Dumper-7 doesn't emit it the way it emits ProcessEventIdx.
    // =========================================================================

    using EngineTickCallback = std::function<void(SDK::UEngine*, float, bool)>;

    class EngineTickHook : public HookBase<EngineTickHook>
    {
        friend class HookBase<EngineTickHook>;

    private:
        struct CallbackEntry
        {
            CallbackHandle      handle;
            EngineTickCallback  callback;
            ExecutionTiming     timing;
            ExecutionMode       mode;
            bool                enabled;

            CallbackEntry(CallbackHandle h, EngineTickCallback cb,
                          ExecutionTiming tim, ExecutionMode mod)
                : handle(h), callback(std::move(cb))
                , timing(tim), mode(mod), enabled(true)
            {
                DBG_ASSERT(h > 0,    "Invalid callback handle");
                DBG_ASSERT(callback, "Callback function is null");
            }
        };

        using CallbackList = std::vector<CallbackEntry>;

        static void(*OriginalTick)(SDK::UEngine*, float, bool);

        std::atomic<std::shared_ptr<const CallbackList>> stateOwner_{};

        EngineTickHook();
        void StoreState(std::shared_ptr<const CallbackList> s);
        static bool RunBeforePass(const CallbackList& list, SDK::UEngine* Eng, float Dt, bool Idle);
        static void RunAfterPass (const CallbackList& list, SDK::UEngine* Eng, float Dt, bool Idle);
        static void __fastcall HookedTick(SDK::UEngine* Engine, float DeltaSeconds, bool bIdleMode);

        // ── CRTP interface ───────────────────────────────────────────────────

        CallbackList CloneList() const;
        void StoreList(CallbackList list);
        void StoreEmptyState();
        bool DoUninstall();

    public:
        // vtableSlot: index of UEngine::Tick in the engine's vtable (per-game).
        bool Install(int32 vtableSlot);

        CallbackHandle AddCallback(EngineTickCallback cb,
            ExecutionTiming timing = ExecutionTiming::Before,
            ExecutionMode  mode   = ExecutionMode::CallOriginal);
    };

    // ── Free convenience functions for the tick hook ─────────────────────────

    bool InstallEngineTickHook(int32 vtableSlot);

    CallbackHandle OnEngineTick(EngineTickCallback cb,
        ExecutionTiming t = ExecutionTiming::Before,
        ExecutionMode mode = ExecutionMode::CallOriginal);

    // =========================================================================
    // StaticConstructObjectHook — hook on StaticConstructObject_Internal.
    //
    //   UObject* StaticConstructObject_Internal(FStaticConstructObjectParameters&)
    //
    // Fires on (almost) every UObject construction, so it is a HOT path and may
    // run on non-game threads (async loading constructs objects too). Keep
    // callbacks cheap and filter early on params->Class. Re-entrancy is guarded
    // per-thread, so constructing objects inside a callback is safe — the nested
    // construction simply won't re-dispatch.
    //
    // No per-callback filters (like EngineTickHook): every registered callback
    // fires for every construction at the requested timing.
    //   Before: params populated, result == nullptr (not constructed yet).
    //   After:  params populated, result == the freshly constructed object.
    //
    // The target address comes from ObjectFactory::GetStaticConstructObject()
    // (per-game RVA / call-site signature in GameOffsets.h). Install() returns
    // false and logs once when the address is unresolved for this game (e.g. RC
    // until its offset is filled in) — every other hook keeps working.
    // =========================================================================

    using StaticConstructObjectCallback =
        std::function<void(FStaticConstructObjectParameters*, SDK::UObject*)>;

    class StaticConstructObjectHook : public HookBase<StaticConstructObjectHook>
    {
        friend class HookBase<StaticConstructObjectHook>;

    private:
        struct CallbackEntry
        {
            CallbackHandle                handle;
            StaticConstructObjectCallback callback;
            ExecutionTiming               timing;
            ExecutionMode                 mode;
            bool                          enabled;

            CallbackEntry(CallbackHandle h, StaticConstructObjectCallback cb,
                          ExecutionTiming tim, ExecutionMode mod)
                : handle(h), callback(std::move(cb))
                , timing(tim), mode(mod), enabled(true)
            {
                DBG_ASSERT(h > 0,    "Invalid callback handle");
                DBG_ASSERT(callback, "Callback function is null");
            }
        };

        using CallbackList = std::vector<CallbackEntry>;

        static SDK::UObject* (*OriginalSCO)(FStaticConstructObjectParameters*);

        std::atomic<std::shared_ptr<const CallbackList>> stateOwner_{};

        StaticConstructObjectHook();
        void StoreState(std::shared_ptr<const CallbackList> s);
        static bool RunBeforePass(const CallbackList& list, FStaticConstructObjectParameters* p);
        static void RunAfterPass (const CallbackList& list, FStaticConstructObjectParameters* p, SDK::UObject* result);
        static SDK::UObject* HookedSCO(FStaticConstructObjectParameters* params);

        // ── CRTP interface ───────────────────────────────────────────────────
        CallbackList CloneList() const;
        void StoreList(CallbackList list);
        void StoreEmptyState();
        bool DoUninstall();

    public:
        bool Install();

        CallbackHandle AddCallback(StaticConstructObjectCallback cb,
            ExecutionTiming timing = ExecutionTiming::After,
            ExecutionMode  mode   = ExecutionMode::CallOriginal);
    };

    // ── Free convenience functions for the SCO hook ──────────────────────────

    bool InstallStaticConstructObjectHook();

    CallbackHandle OnStaticConstructObject(StaticConstructObjectCallback cb,
        ExecutionTiming t = ExecutionTiming::After,
        ExecutionMode mode = ExecutionMode::CallOriginal);

    // =========================================================================
    // HookToggle<Hook> — a toggleable hook-callback registration.
    //
    // Wraps the "store a CallbackHandle; register on enable, remove on disable"
    // dance repeated all over Commands.cpp. Bind it to a hook type and give it a
    // registrar that returns a handle:
    //
    //   static HookToggle<ProcessEventHook> g_tick{
    //       [] { return OnProcessEventByName("ReceiveTick", &OnTick); } };
    //   ...
    //   g_tick.Toggle();          // flips, returns the resulting enabled state
    //   g_tick.Enable();  g_tick.Disable();  g_tick.Set(on);  g_tick.IsEnabled();
    //
    // Disable() removes via Hook::Get().RemoveCallback(); the handle resets to 0 so
    // Enable() re-registers cleanly. Every live toggle is reset by ResetAllToggles()
    // (called from ModManager::UnloadMods), so you no longer hand-zero handles.
    // =========================================================================

    class ToggleBase
    {
    public:
        // Forget the current handle WITHOUT removing the callback (the hook
        // teardown that follows clears it). After this, IsEnabled() == false.
        virtual void Reset() noexcept = 0;
    protected:
        ToggleBase();
        ~ToggleBase();
        ToggleBase(const ToggleBase&)            = delete;
        ToggleBase& operator=(const ToggleBase&) = delete;
    };

    // Reset every live HookToggle (forget handles). Called on hook teardown/reload.
    void ResetAllToggles() noexcept;

    template<typename Hook>
    class HookToggle : public ToggleBase
    {
        CallbackHandle                  handle_ = 0;
        std::function<CallbackHandle()> register_;

    public:
        explicit HookToggle(std::function<CallbackHandle()> registrar)
            : register_(std::move(registrar)) {}

        bool IsEnabled() const noexcept { return handle_ != 0; }

        // Each returns the resulting enabled state.
        bool Enable()
        {
            if (!handle_ && register_) handle_ = register_();
            return handle_ != 0;
        }
        bool Disable()
        {
            if (handle_) { Hook::Get().RemoveCallback(handle_); handle_ = 0; }
            return false;
        }
        bool Set(bool on) { return on ? Enable() : Disable(); }
        bool Toggle()     { return handle_ ? Disable() : Enable(); }

        void Reset() noexcept override { handle_ = 0; }
    };

    // =========================================================================
    // ScopedHookToggle<Hook> — RAII HookToggle.
    //
    // Enables the registrar on construction and removes the callback on
    // destruction — for temporary / diagnostic hooks that should only live for a
    // scope (e.g. a one-shot pewatch session). It owns a HookToggle, so it still
    // participates in ResetAllToggles(): if the hooks tear down first (DLL unload
    // / reload), the handle is already forgotten and ~ScopedHookToggle's Disable()
    // is a safe no-op.
    //
    //   {
    //       GameHooks::ScopedHookToggle<GameHooks::ProcessEventHook> watch{
    //           [] { return GameHooks::OnProcessEventAll(&LogIt); } };
    //       ... callback active only for this scope ...
    //   } // automatically removed here
    // =========================================================================
    template<typename Hook>
    class ScopedHookToggle
    {
        HookToggle<Hook> toggle_;
    public:
        explicit ScopedHookToggle(std::function<CallbackHandle()> registrar, bool enableNow = true)
            : toggle_(std::move(registrar)) { if (enableNow) toggle_.Enable(); }
        ~ScopedHookToggle() { toggle_.Disable(); }

        ScopedHookToggle(const ScopedHookToggle&)            = delete;
        ScopedHookToggle& operator=(const ScopedHookToggle&) = delete;

        bool Enable()                   { return toggle_.Enable(); }
        bool Disable()                  { return toggle_.Disable(); }
        bool Toggle()                   { return toggle_.Toggle(); }
        bool Set(bool on)               { return toggle_.Set(on); }
        bool IsEnabled() const noexcept { return toggle_.IsEnabled(); }
    };

} // namespace GameHooks

// =========================================================================
// EnqueueOnce — runs fn once on the game thread on the next EngineTick.
// =========================================================================

void EnqueueOnce(std::function<void()> fn);

// =========================================================================
// EnqueueWhile — runs fn on the game thread every tick until fn returns false.
// =========================================================================

void EnqueueWhile(std::function<bool()> fn);

// =========================================================================
// EnqueueEveryNTicks — runs fn on the game thread every N engine ticks until
//   fn returns false (or the returned handle is passed to RemoveCallback).
//
// Unlike EnqueueOnce/EnqueueWhile (which use the task-drain queue), this
// registers a proper named tick callback, giving full ExecutionTiming control
// (Before / After / BeforeAndAfter) and a removable handle.
//
// n == 0 is treated as n == 1 (runs every tick, same as EnqueueWhile but
// with timing and a handle).
//
// Example:
//   auto h = EnqueueEveryNTicks(10, []{ DoSomething(); return true; });
//   // later:
//   GameHooks::EngineTickHook::Get().RemoveCallback(h);
// =========================================================================

GameHooks::CallbackHandle EnqueueEveryNTicks(
    uint32_t                   n,
    std::function<bool()>      fn,
    GameHooks::ExecutionTiming timing = GameHooks::ExecutionTiming::After);

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
    auto& hook = GameHooks::EngineTickHook::Get();

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

    auto& hook  = GameHooks::EngineTickHook::Get();
    auto fireAt = std::make_shared<steady_clock::time_point>(steady_clock::now() + ms);
    auto task   = std::make_shared<std::function<void()>>();

    *task = [fireAt, fn = std::move(fn), task, &hook]() mutable
    {
        if (steady_clock::now() < *fireAt) { hook.Enqueue(*task); return; }
        fn();
    };

    hook.Enqueue(*task);
}
