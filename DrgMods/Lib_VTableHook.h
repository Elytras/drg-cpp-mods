#pragma once
// Lib_VTableHook.h — Simplified per-slot vtable hook with a central registry.
//
// Design:
//   - No per-callback filtering (no class/object/function filters).
//   - Before / After timing only.
//   - Non-void slots: Before callbacks receive a RetVal& and may return
//     ExecutionMode::SkipOriginal to bypass the real function.
//     After callbacks receive the final RetVal& (read or write).
//   - VTableHookRegistry is a Meyers-singleton that owns every installed slot
//     hook. Each (Slot, Sig) pair has exactly one SlotHook instance.
//
// Quick example:
//   auto& reg = VTH::VTableHookRegistry::Get();
//   reg.Install<Slots::IsAsset, Sigs::FIsAsset>(engine);
//
//   auto h = reg.Hook<Slots::IsAsset, Sigs::FIsAsset>().Before(
//       [](const UObject* Obj, bool& ret) {
//           ret = true;
//           return VTH::ExecutionMode::SkipOriginal;
//       });
//
//   reg.Hook<Slots::BeginDestroy, Sigs::FBeginDestroy>().Before(
//       [](UObject* Obj) { return VTH::ExecutionMode::CallOriginal; });

#include <atomic>
#include <cassert>
#include <functional>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <Windows.h>
#include "Lib_Forward.h"
#include "Lib_Utils.h"
#include "Lib_EasyHook.h"
#include "Common.h"
#ifdef HOOKALIASES
#include "Lib_VTableINfo.h"
#endif
#ifdef _DEBUG
#define VH_ASSERT(cond, msg) assert((cond) && (msg))
#define VH_CHECK_PTR(ptr)    assert((ptr) != nullptr && "Null pointer")
#else
#define VH_ASSERT(cond, msg) ((void)0)
#define VH_CHECK_PTR(ptr)    ((void)0)
#endif

namespace VTH
{
    using namespace SDK;

    // =========================================================================
    //  Shared types
    // =========================================================================

    enum class ExecutionMode { CallOriginal, SkipOriginal };

    using Handle = size_t;
    static constexpr Handle kInvalidHandle = 0;

    // =========================================================================
    //  Sig traits
    // =========================================================================
    namespace Detail
    {
        template<typename Sig> struct SigTraits;
        template<typename R, typename... Args>
        struct SigTraits<R(Args...)>
        {
            using Ret = R;
            using IsVoid = std::is_void<R>;
        };

        // Callback shapes:
        //   Before (non-void): fn(Args..., Ret&) -> ExecutionMode
        //   Before (void):     fn(Args...)       -> ExecutionMode
        //   After  (non-void): fn(Args..., Ret&) -> void
        //   After  (void):     fn(Args...)       -> void
        template<typename Sig, bool IsVoid> struct BeforeCB;
        template<typename Sig, bool IsVoid> struct AfterCB;

        template<typename R, typename... Args>
        struct BeforeCB<R(Args...), false> { using Type = std::function<ExecutionMode(Args..., R&)>; };
        template<typename R, typename... Args>
        struct BeforeCB<R(Args...), true> { using Type = std::function<ExecutionMode(Args...)>; };
        template<typename R, typename... Args>
        struct AfterCB<R(Args...), false> { using Type = std::function<void(Args..., R&)>; };
        template<typename R, typename... Args>
        struct AfterCB<R(Args...), true> { using Type = std::function<void(Args...)>; };

        template<typename R> R ZeroRet() { return R{}; }

    } // namespace Detail

    // =========================================================================
    //  ISlotHook — type-erased base stored in the registry
    // =========================================================================
    struct ISlotHook
    {
        virtual ~ISlotHook() = default;
        virtual bool IsInstalled()        const = 0;
        virtual bool Install(UObject*) = 0;
        virtual bool RemoveCallback(Handle h) = 0;
    };

    // =========================================================================
    //  SlotHook<Slot, Sig>
    // =========================================================================
    template<int32_t Slot, typename Sig>
    class SlotHook final : public ISlotHook
    {
    public:
        using Traits = Detail::SigTraits<Sig>;
        using Ret = typename Traits::Ret;
        static constexpr bool kVoid = Traits::IsVoid::value;

        using BeforeFn = typename Detail::BeforeCB<Sig, kVoid>::Type;
        using AfterFn = typename Detail::AfterCB<Sig, kVoid>::Type;

    private:
        struct BeforeEntry { Handle h; BeforeFn fn; };
        struct AfterEntry { Handle h; AfterFn  fn; };

        mutable std::mutex       mutex_;
        std::vector<BeforeEntry> before_;
        std::vector<AfterEntry>  after_;
        std::atomic<bool>        installed_{ false };
        Handle                   next_{ 1 };

        static SlotHook* instance_;
        static Sig* Original_;

        SlotHook() = default;

        // ── Trampoline bodies — one per void/non-void branch ──────────────────
        // Concrete arg types are provided by the partial-spec helpers below.

        template<typename... Args>
        static Ret RunNonVoid(Args... args)
        {
            auto* inst = instance_;
            if (!inst || !inst->installed_.load(std::memory_order_relaxed))
                return Original_ ? Original_(args...) : Detail::ZeroRet<Ret>();

            Ret  retVal = Detail::ZeroRet<Ret>();
            bool callOrig = true;

            {
                std::lock_guard lock(inst->mutex_);
                for (auto& e : inst->before_)
                    if (e.fn(args..., retVal) == ExecutionMode::SkipOriginal)
                    {
                        callOrig = false; break;
                    }
            }
            if (callOrig && Original_)
                retVal = Original_(args...);
            {
                std::lock_guard lock(inst->mutex_);
                for (auto& e : inst->after_)
                    e.fn(args..., retVal);
            }
            return retVal;
        }

        template<typename... Args>
        static void RunVoid(Args... args)
        {
            auto* inst = instance_;
            if (!inst || !inst->installed_.load(std::memory_order_relaxed))
            {
                if (Original_) Original_(args...); return;
            }

            bool callOrig = true;
            {
                std::lock_guard lock(inst->mutex_);
                for (auto& e : inst->before_)
                    if (e.fn(args...) == ExecutionMode::SkipOriginal)
                    {
                        callOrig = false; break;
                    }
            }
            if (callOrig && Original_) Original_(args...);
            {
                std::lock_guard lock(inst->mutex_);
                for (auto& e : inst->after_)
                    e.fn(args...);
            }
        }

        // ── Trampoline — unpacks Sig's arg list via partial specialisation ────
        // TrampolineFor<Sig, kVoid>::Fn is the static function we give EasyHook.

        template<typename S, bool V> struct TrampolineFor;

        template<typename R, typename... Args>
        struct TrampolineFor<R(Args...), /*kVoid=*/false>
        {
            static R Fn(Args... args) { return SlotHook::RunNonVoid(args...); }
        };
        template<typename R, typename... Args>
        struct TrampolineFor<R(Args...), /*kVoid=*/true>
        {
            static void Fn(Args... args) { SlotHook::RunVoid(args...); }
        };

    public:
        static SlotHook& Get()
        {
            if (!instance_) instance_ = new SlotHook();
            return *instance_;
        }

        bool IsInstalled() const override { return installed_.load(); }

        bool Install(UObject* sample) override
        {
            VH_CHECK_PTR(sample);
            if (installed_.load()) return true;
            if (!EasyHook::Init()) return false;

            void* addr = Utils::GetVirtualFunction<void*>(sample, Slot);
            VH_CHECK_PTR(addr);

            void* tramp = reinterpret_cast<void*>(&TrampolineFor<Sig, kVoid>::Fn);
            if (!EasyHook::CreateAndEnableHook(addr, tramp,
                reinterpret_cast<void**>(&Original_)))
            {
                VH_ASSERT(false, "EasyHook failed");
                return false;
            }
            VH_CHECK_PTR(Original_);
            installed_.store(true);
            return true;
        }

        Handle Before(BeforeFn fn)
        {
            VH_ASSERT(fn, "Null before-callback");
            std::lock_guard lock(mutex_);
            Handle h = next_++;
            before_.push_back({ h, std::move(fn) });
            return h;
        }

        Handle After(AfterFn fn)
        {
            VH_ASSERT(fn, "Null after-callback");
            std::lock_guard lock(mutex_);
            Handle h = next_++;
            after_.push_back({ h, std::move(fn) });
            return h;
        }

        bool RemoveCallback(Handle h) override
        {
            VH_ASSERT(h != kInvalidHandle, "Invalid handle");
            std::lock_guard lock(mutex_);
            auto erase = [&](auto& vec) -> bool {
                auto it = std::remove_if(vec.begin(), vec.end(),
                    [h](const auto& e) { return e.h == h; });
                if (it == vec.end()) return false;
                vec.erase(it, vec.end()); return true;
                };
            return erase(before_) || erase(after_);
        }

        void   ClearBefore() { std::lock_guard lock(mutex_); before_.clear(); }
        void   ClearAfter() { std::lock_guard lock(mutex_); after_.clear(); }
        void   ClearAll() { std::lock_guard lock(mutex_); before_.clear(); after_.clear(); }
        size_t BeforeCount() const { std::lock_guard lock(mutex_); return before_.size(); }
        size_t AfterCount()  const { std::lock_guard lock(mutex_); return after_.size(); }
    };

    template<int32_t S, typename Sig> SlotHook<S, Sig>* SlotHook<S, Sig>::instance_ = nullptr;
    template<int32_t S, typename Sig> Sig* SlotHook<S, Sig>::Original_ = nullptr;

    // =========================================================================
    //  VTableHookRegistry
    // =========================================================================
    class VTableHookRegistry
    {
        mutable std::mutex                       mutex_;
        std::unordered_map<int32_t, ISlotHook*> slots_;
        VTableHookRegistry() = default;

    public:
        static VTableHookRegistry& Get()
        {
            static VTableHookRegistry inst;
            return inst;
        }

        // Install a slot hook. Idempotent.
        template<int32_t Slot, typename Sig>
        bool Install(UObject* sample)
        {
            VH_CHECK_PTR(sample);
            auto& hook = SlotHook<Slot, Sig>::Get();
            { std::lock_guard lock(mutex_); slots_.emplace(Slot, &hook); }
            return hook.Install(sample);
        }

        // Typed access to a slot's hook. Asserts that Install was called first.
        template<int32_t Slot, typename Sig>
        SlotHook<Slot, Sig>& Hook()
        {
            VH_ASSERT(SlotHook<Slot, Sig>::Get().IsInstalled(),
                "Hook() called before Install() for this slot");
            return SlotHook<Slot, Sig>::Get();
        }

        // Scan all slots to remove by handle. O(slots).
        // Use the typed hook's RemoveCallback for O(n callbacks).
        bool RemoveCallback(Handle h)
        {
            VH_ASSERT(h != kInvalidHandle, "Invalid handle");
            std::lock_guard lock(mutex_);
            for (auto& [slot, hook] : slots_)
                if (hook->RemoveCallback(h)) return true;
            return false;
        }

        bool   IsInstalled(int32_t slot) const
        {
            std::lock_guard lock(mutex_);
            auto it = slots_.find(slot);
            return it != slots_.end() && it->second->IsInstalled();
        }
        size_t InstalledSlotCount() const
        {
            std::lock_guard lock(mutex_);
            return slots_.size();
        }
    };
#ifdef HOOKALIASES
    // =========================================================================
    //  Named aliases — Hooks::<Name>Hook resolves to the right SlotHook<>
    // =========================================================================
    namespace Hooks
    {
        using namespace VTableLayout::Slots;
        using namespace VTableLayout::Signatures;

        // UObjectBase
        using VecDelDtorHook = SlotHook<VecDelDtor, FVecDelDtor>;
        using RegisterDependenciesHook = SlotHook<RegisterDependencies, FRegisterDependencies>;
        using DeferredRegisterHook = SlotHook<DeferredRegister, FDeferredRegister>;
        // UObjectBaseUtility
        using CanBeClusterRootHook = SlotHook<CanBeClusterRoot, FCanBeClusterRoot>;
        using CanBeInClusterHook = SlotHook<CanBeInCluster, FCanBeInCluster>;
        using CreateClusterHook = SlotHook<CreateCluster, FCreateCluster>;
        using OnClusterMarkedAsPendingKillHook = SlotHook<OnClusterMarkedAsPendingKill, FOnClusterMarkedAsPendingKill>;
        // UObject: safe range
        using GetDetailedInfoInternalHook = SlotHook<GetDetailedInfoInternal, FGetDetailedInfoInternal>;
        using PostInitPropertiesHook = SlotHook<PostInitProperties, FPostInitProperties>;
        using PostCDOContructHook = SlotHook<PostCDOContruct, FPostCDOContruct>;
        using PreSaveRootHook = SlotHook<PreSaveRoot, FPreSaveRoot>;
        using PostSaveRootHook = SlotHook<PostSaveRoot, FPostSaveRoot>;
        using PreSaveHook = SlotHook<PreSave, FPreSave>;
        using IsReadyForAsyncPostLoadHook = SlotHook<IsReadyForAsyncPostLoad, FIsReadyForAsyncPostLoad>;
        using PostLoadHook = SlotHook<PostLoad, FPostLoad>;
        using PostLoadSubobjectsHook = SlotHook<PostLoadSubobjects, FPostLoadSubobjects>;
        using BeginDestroyHook = SlotHook<BeginDestroy, FBeginDestroy>;
        using IsReadyForFinishDestroyHook = SlotHook<IsReadyForFinishDestroy, FIsReadyForFinishDestroy>;
        using FinishDestroyHook = SlotHook<FinishDestroy, FFinishDestroy>;
        using Serialize_ArchiveHook = SlotHook<Serialize_Archive, FSerialize_Archive>;
        using Serialize_StructuredHook = SlotHook<Serialize_Structured, FSerialize_Structured>;
        using ShutdownAfterErrorHook = SlotHook<ShutdownAfterError, FShutdownAfterError>;
        using PostInterpChangeHook = SlotHook<PostInterpChange, FPostInterpChange>;
        // UObject: unsafe / shipping range
        using PostRenameHook = SlotHook<PostRename, FPostRename>;
        using PreDuplicateHook = SlotHook<PreDuplicate, FPreDuplicate>;
        // PostDuplicate_Mode skipped — EDuplicateMode::Type unavailable
        using PostDuplicate_BoolHook = SlotHook<PostDuplicate_Bool, FPostDuplicate_Bool>;
        using NeedsLoadForClientHook = SlotHook<NeedsLoadForClient, FNeedsLoadForClient>;
        using NeedsLoadForServerHook = SlotHook<NeedsLoadForServer, FNeedsLoadForServer>;
        using NeedsLoadForTargetPlatformHook = SlotHook<NeedsLoadForTargetPlatform, FNeedsLoadForTargetPlatform>;
        using NeedsLoadForEditorGameHook = SlotHook<NeedsLoadForEditorGame, FNeedsLoadForEditorGame>;
        using IsEditorOnlyHook = SlotHook<IsEditorOnly, FIsEditorOnly>;
        using HasNonEditorOnlyReferencesHook = SlotHook<HasNonEditorOnlyReferences, FHasNonEditorOnlyReferences>;
        using IsPostLoadThreadSafeHook = SlotHook<IsPostLoadThreadSafe, FIsPostLoadThreadSafe>;
        using IsDestructionThreadSafeHook = SlotHook<IsDestructionThreadSafe, FIsDestructionThreadSafe>;
        using GetPreloadDependenciesHook = SlotHook<GetPreloadDependencies, FGetPreloadDependencies>;
        using GetPrestreamPackagesHook = SlotHook<GetPrestreamPackages, FGetPrestreamPackages>;
        using ExportCustomPropertiesHook = SlotHook<ExportCustomProperties, FExportCustomProperties>;
        using ImportCustomPropertiesHook = SlotHook<ImportCustomProperties, FImportCustomProperties>;
        using PostEditImportHook = SlotHook<PostEditImport, FPostEditImport>;
        using PostReloadConfigHook = SlotHook<PostReloadConfig, FPostReloadConfig>;
        using RenameHook = SlotHook<Rename, FRename>;
        using GetDescHook = SlotHook<GetDesc, FGetDesc>;
        using GetSparseClassDataStructHook = SlotHook<GetSparseClassDataStruct, FGetSparseClassDataStruct>;
        using GetWorldHook = SlotHook<GetWorld, FGetWorld>;
        using GetNativePropertyValuesHook = SlotHook<GetNativePropertyValues, FGetNativePropertyValues>;
        using GetResourceSizeExHook = SlotHook<GetResourceSizeEx, FGetResourceSizeEx>;
        using GetExporterNameHook = SlotHook<GetExporterName, FGetExporterName>;
        // GetRestoreForUObjectOverwrite skipped — FRestoreForUObjectOverwrite unavailable
        using AreNativePropertiesIdenticalToHook = SlotHook<AreNativePropertiesIdenticalTo, FAreNativePropertiesIdenticalTo>;
        using GetAssetRegistryTagsHook = SlotHook<GetAssetRegistryTags, FGetAssetRegistryTags>;
        using IsAssetHook = SlotHook<IsAsset, FIsAsset>;
        using GetPrimaryAssetIdHook = SlotHook<GetPrimaryAssetId, FGetPrimaryAssetId>;
        using IsLocalizedResourceHook = SlotHook<IsLocalizedResource, FIsLocalizedResource>;
        using IsSafeForRootSetHook = SlotHook<IsSafeForRootSet, FIsSafeForRootSet>;
        using TagSubobjectsHook = SlotHook<TagSubobjects, FTagSubobjects>;
        using GetLifetimeReplicatedPropsHook = SlotHook<GetLifetimeReplicatedProps, FGetLifetimeReplicatedProps>;
        using IsNameStableForNetworkingHook = SlotHook<IsNameStableForNetworking, FIsNameStableForNetworking>;
        using IsFullNameStableForNetworkingHook = SlotHook<IsFullNameStableForNetworking, FIsFullNameStableForNetworking>;
        using IsSupportedForNetworkingHook = SlotHook<IsSupportedForNetworking, FIsSupportedForNetworking>;
        using GetSubobjectsWithStableNamesHook = SlotHook<GetSubobjectsWithStableNamesForNetworking, FGetSubobjectsWithStableNamesForNetworking>;
        using PreNetReceiveHook = SlotHook<PreNetReceive, FPreNetReceive>;
        using PostNetReceiveHook = SlotHook<PostNetReceive, FPostNetReceive>;
        using PostRepNotifiesHook = SlotHook<PostRepNotifies, FPostRepNotifies>;
        using PreDestroyFromReplicationHook = SlotHook<PreDestroyFromReplication, FPreDestroyFromReplication>;
        using BuildSubobjectMappingHook = SlotHook<BuildSubobjectMapping, FBuildSubobjectMapping>;
        using GetConfigOverridePlatformHook = SlotHook<GetConfigOverridePlatform, FGetConfigOverridePlatform>;
        using OverridePerObjectConfigSectionHook = SlotHook<OverridePerObjectConfigSection, FOverridePerObjectConfigSection>;
        using ProcessEventHook = SlotHook<ProcessEvent, FProcessEvent>;
        using GetFunctionCallspaceHook = SlotHook<GetFunctionCallspace, FGetFunctionCallspace>;
        using CallRemoteFunctionHook = SlotHook<CallRemoteFunction, FCallRemoteFunction>;
        using ProcessConsoleExecHook = SlotHook<ProcessConsoleExec, FProcessConsoleExec>;
        using RegenerateClassHook = SlotHook<RegenerateClass, FRegenerateClass>;
        using MarkAsEditorOnlySubobjectHook = SlotHook<MarkAsEditorOnlySubobject, FMarkAsEditorOnlySubobject>;
        using CheckDefaultSubobjectsInternalHook = SlotHook<CheckDefaultSubobjectsInternal, FCheckDefaultSubobjectsInternal>;
        using ValidateGeneratedRepEnumsHook = SlotHook<ValidateGeneratedRepEnums, FValidateGeneratedRepEnums>;
        using SetNetPushIdDynamicHook = SlotHook<SetNetPushIdDynamic, FSetNetPushIdDynamic>;
        using GetNetPushIdDynamicHook = SlotHook<GetNetPushIdDynamic, FGetNetPushIdDynamic>;
    } // namespace Hooks
#endif
} // namespace VTH


// =============================================================================
//  Usage examples  (compiled only when VTH_EXAMPLES is defined)
// =============================================================================
#ifdef VTH_EXAMPLES

void Examples(SDK::UObject* engine)
{
    using namespace SDK;
    using namespace VTableLayout;
    auto& reg = VTH::VTableHookRegistry::Get();

    // ── 1. Override IsAsset (non-void, SkipOriginal) ──────────────────────────
    reg.Install<Slots::IsAsset, Signatures::FIsAsset>(engine);

    VTH::Handle hIsAsset =
        reg.Hook<Slots::IsAsset, Signatures::FIsAsset>()
        .Before([](const UObject*, bool& ret) {
        ret = true;
        return VTH::ExecutionMode::SkipOriginal;
            });

    // ── 2. Observe BeginDestroy (void, no override) ───────────────────────────
    reg.Install<Slots::BeginDestroy, Signatures::FBeginDestroy>(engine);

    reg.Hook<Slots::BeginDestroy, Signatures::FBeginDestroy>()
        .Before([](UObject*) {
        return VTH::ExecutionMode::CallOriginal;
            });

    // ── 3. After-pass — read/patch GetWorld's return value ────────────────────
    reg.Install<Slots::GetWorld, Signatures::FGetWorld>(engine);

    reg.Hook<Slots::GetWorld, Signatures::FGetWorld>()
        .After([](const UObject*, UWorld*& world) {
        (void)world; // inspect or replace
            });

    // ── 4. Named alias — no template args at the call site ────────────────────
    VTH::Hooks::IsAssetHook::Get()
        .Before([](const UObject*, bool& ret) {
        ret = false;
        return VTH::ExecutionMode::SkipOriginal;
            });

    // ── 5. Remove via typed hook (O(n callbacks)) ─────────────────────────────
    reg.Hook<Slots::IsAsset, Signatures::FIsAsset>().RemoveCallback(hIsAsset);

    // ── 6. Remove via registry scan (O(slots)) ────────────────────────────────
    reg.RemoveCallback(hIsAsset);
}

#endif // VTH_EXAMPLES