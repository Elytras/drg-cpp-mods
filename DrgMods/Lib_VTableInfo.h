#pragma once
#include "Common.h"
#include "StringLib.h"
#include "SDK/UtfN.hpp"
#include "SDK/UnrealContainers.hpp"
#include "SDK/SDK/Basic.hpp"
#include "Lib_Forward.h"
#include "Lib_FField.h"

using ERenameFlags = uint32;
namespace REN
{
    constexpr ERenameFlags None = 0x0000;
    constexpr ERenameFlags ForceNoResetLoaders = 0x0001;
    constexpr ERenameFlags Test = 0x0002;
    constexpr ERenameFlags DoNotDirty = 0x0004;
    constexpr ERenameFlags DontCreateRedirectors = 0x0010;
    constexpr ERenameFlags NonTransactional = 0x0020;
    constexpr ERenameFlags ForceGlobalUnique = 0x0040;
    constexpr ERenameFlags SkipGeneratedClasses = 0x0080;
}
namespace VTableLayout
{
    using namespace UC;
    // ─────────────────────────────────────────────────────────────────────────
    //  Function signature types
    //  Convention: explicit this pointer, x64 MSVC.
    //  FString/FName/FPrimaryAssetId returned via hidden output pointer (NRVO).
    // ─────────────────────────────────────────────────────────────────────────
    namespace Signatures
    {
        using SDK::UObject;
        using SDK::FName;
        using UC::FString;
        
        // ── UObjectBase ───────────────────────────────────────────────────────
        using FVecDelDtor = void* (UObject*, uint32);
        using FRegisterDependencies = void(UObject*);
        using FDeferredRegister = void(UObject*, SDK::UClass*, const TCHAR*, const TCHAR*);

        // ── UObjectBaseUtility ────────────────────────────────────────────────
        using FCanBeClusterRoot = bool(const UObject*);
        using FCanBeInCluster = bool(const UObject*);
        using FCreateCluster = void(UObject*);
        using FOnClusterMarkedAsPendingKill = void(UObject*);

        // ── UObject ───────────────────────────────────────────────────────────
        using FGetDetailedInfoInternal = UC::FString*(UObject*, FString*);
        using FPostInitProperties = void(UObject*);
        using FPostCDOContruct = void(UObject*);
        using FPreSaveRoot = bool(UObject*, const TCHAR*);
        using FPostSaveRoot = void(UObject*, bool);
        using FPreSave = void(UObject*, const void* /* ITargetPlatform */);
        using FIsReadyForAsyncPostLoad = bool(const UObject*);
        using FPostLoad = void(UObject*);
        using FPostLoadSubobjects = void(UObject*, void* /* SDK::FObjectInstancingGraph */);
        using FBeginDestroy = void(UObject*);
        using FIsReadyForFinishDestroy = bool(UObject*);
        using FFinishDestroy = void(UObject*);
        using FSerialize_Archive = void(UObject*, void* /*SDK::FArchive&*/);
        using FSerialize_Structured = void(UObject*, void* /*SDK::FStructuredArchive::FRecord*/);
        using FShutdownAfterError = void(UObject*);
        using FPostInterpChange = void(UObject*, SDK::FProperty*);
        using FPostRename = void(UObject*, UObject*, const FName);
        using FPreDuplicate = void(UObject*, void* /* FObjectDuplicationParameters& */);
        //using FPostDuplicate_Mode = void(UObject*, EDuplicateMode::Type);
        using FPostDuplicate_Bool = void(UObject*, bool);
        using FNeedsLoadForClient = bool(const UObject*);
        using FNeedsLoadForServer = bool(const UObject*);
        using FNeedsLoadForTargetPlatform = bool(const UObject*, const void* /* ITargetPlatform */);
        using FNeedsLoadForEditorGame = bool(const UObject*);
        using FIsEditorOnly = bool(const UObject*);
        using FHasNonEditorOnlyReferences = bool(const UObject*);
        using FIsPostLoadThreadSafe = bool(const UObject*);
        using FIsDestructionThreadSafe = bool(const UObject*);
        using FGetPreloadDependencies = void(UObject*, TArray<UObject*>&);
        using FGetPrestreamPackages = void(UObject*, TArray<UObject*>&);
        using FExportCustomProperties = void(UObject*, void* /* FOutputDevice& */, uint32);
        using FImportCustomProperties = void(UObject*, const TCHAR*, void* /* FFeedbackContext* */);
        using FPostEditImport = void(UObject*);
        using FPostReloadConfig = void(UObject*, FProperty*);
        using FRename = bool(UObject*, const TCHAR*, UObject*, ERenameFlags);
        using FGetDesc = FString*(UObject*, FString*);
        using FGetSparseClassDataStruct = UScriptStruct * (const UObject*);
        using FGetWorld = UWorld * (const UObject*);
        using FGetNativePropertyValues = bool(const UObject*, TMap<FString, FString>&, uint32);
        using FGetResourceSizeEx = void(UObject*, void* /* FResourceSizeEx& */);
        using FGetExporterName = FName * (UObject*, FName*);
        //using FGetRestoreForUObjectOverwrite = FRestoreForUObjectOverwrite * (UObject*);
        using FAreNativePropertiesIdenticalTo = bool(const UObject*, UObject*);
        using FGetAssetRegistryTags = void(const UObject*, void* /* TArray<UObject::FAssetRegistryTag>& */);
        using FIsAsset = bool(const UObject*);
        using FGetPrimaryAssetId = FPrimaryAssetId * (const UObject*, FPrimaryAssetId*);
        using FIsLocalizedResource = bool(const UObject*);
        using FIsSafeForRootSet = bool(const UObject*);
        using FTagSubobjects = void(UObject*, EObjectFlags);
        using FGetLifetimeReplicatedProps = void(const UObject*, TArray<FLifetimeProperty>& );
        using FIsNameStableForNetworking = bool(const UObject*);
        using FIsFullNameStableForNetworking = bool(const UObject*);
        using FIsSupportedForNetworking = bool(const UObject*);
        using FGetSubobjectsWithStableNamesForNetworking = void(UObject*, TArray<UObject*>&);
        using FPreNetReceive = void(UObject*);
        using FPostNetReceive = void(UObject*);
        using FPostRepNotifies = void(UObject*);
        using FPreDestroyFromReplication = void(UObject*);
        using FBuildSubobjectMapping = void(const UObject*, UObject*, TMap<UObject*, UObject*>&);
        using FGetConfigOverridePlatform = const TCHAR* (const UObject*);
        using FOverridePerObjectConfigSection = void(UObject*, FString&);
        using FProcessEvent = void(UObject*, UFunction*, void*);
        using FGetFunctionCallspace = int32(UObject*, UFunction*, void* /* FFrame* */);
        using FCallRemoteFunction = bool(UObject*, UFunction*, void*, void* /* FOutParmRec* */, void* /* FFrame* */);
        using FProcessConsoleExec = bool(UObject*, const TCHAR*, void* /* FOutputDevice& */, UObject*);
        using FRegenerateClass = UClass*(UObject*, UClass*, UObject*);
        using FMarkAsEditorOnlySubobject = void(UObject*);
        using FCheckDefaultSubobjectsInternal = bool(const UObject*);
        using FValidateGeneratedRepEnums = void(const UObject*, const void* /* TArray<FRepRecord>& */);
        using FSetNetPushIdDynamic = void(UObject*, int32);
        using FGetNetPushIdDynamic = int32(const UObject*);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Absolute vtable slots — shipping build (FSD-Win64-Shipping.exe).
    //  WITH_EDITOR blocks are compiled out; slot order verified against
    //  source declaration order and confirmed by vtable dump.
    // ─────────────────────────────────────────────────────────────────────────
    namespace Slots
    {
        // ── UObjectBase (0-2) ─────────────────────────────────────────────────
        constexpr int32 VecDelDtor = 0;
        constexpr int32 RegisterDependencies = 1;
        constexpr int32 DeferredRegister = 2;

        // ── UObjectBaseUtility (3-6) ──────────────────────────────────────────
        constexpr int32 CanBeClusterRoot = 3;
        constexpr int32 CanBeInCluster = 4;
        constexpr int32 CreateCluster = 5;
        constexpr int32 OnClusterMarkedAsPendingKill = 6;

        // ── UObject (7-77) ────────────────────────────────────────────────────
        constexpr int32 GetDetailedInfoInternal = 7;
        constexpr int32 PostInitProperties = 8;
        constexpr int32 PostCDOContruct = 9;
        constexpr int32 PreSaveRoot = 10;
        constexpr int32 PostSaveRoot = 11;
        constexpr int32 PreSave = 12;
        constexpr int32 IsReadyForAsyncPostLoad = 13;
        constexpr int32 PostLoad = 14;
        constexpr int32 PostLoadSubobjects = 15;
        constexpr int32 BeginDestroy = 16;
        constexpr int32 IsReadyForFinishDestroy = 17;
        constexpr int32 FinishDestroy = 18;
        constexpr int32 Serialize_Archive = 19;
        constexpr int32 Serialize_Structured = 20;
        constexpr int32 ShutdownAfterError = 21;
        constexpr int32 PostInterpChange = 22;
        constexpr int32 PostRename = 23;
        constexpr int32 PreDuplicate = 24;
        constexpr int32 PostDuplicate_Mode = 25;
        constexpr int32 PostDuplicate_Bool = 26;
        constexpr int32 NeedsLoadForClient = 27;
        constexpr int32 NeedsLoadForServer = 28;
        constexpr int32 NeedsLoadForTargetPlatform = 29;
        constexpr int32 NeedsLoadForEditorGame = 30;
        constexpr int32 IsEditorOnly = 31;
        constexpr int32 HasNonEditorOnlyReferences = 32;
        constexpr int32 IsPostLoadThreadSafe = 33;
        constexpr int32 IsDestructionThreadSafe = 34;
        constexpr int32 GetPreloadDependencies = 35;
        constexpr int32 GetPrestreamPackages = 36;
        constexpr int32 ExportCustomProperties = 37;
        constexpr int32 ImportCustomProperties = 38;
        constexpr int32 PostEditImport = 39;
        constexpr int32 PostReloadConfig = 40;
        constexpr int32 Rename = 41;
        constexpr int32 GetDesc = 42;
        constexpr int32 GetSparseClassDataStruct = 43;
        constexpr int32 GetWorld = 44;
        constexpr int32 GetNativePropertyValues = 45;
        constexpr int32 GetResourceSizeEx = 46;
        constexpr int32 GetExporterName = 47;
        constexpr int32 GetRestoreForUObjectOverwrite = 48;
        constexpr int32 AreNativePropertiesIdenticalTo = 49;
        constexpr int32 GetAssetRegistryTags = 50;
        constexpr int32 IsAsset = 51;
        constexpr int32 GetPrimaryAssetId = 52;
        constexpr int32 IsLocalizedResource = 53;
        constexpr int32 IsSafeForRootSet = 54;
        constexpr int32 TagSubobjects = 55;
        constexpr int32 GetLifetimeReplicatedProps = 56;
        constexpr int32 IsNameStableForNetworking = 57;
        constexpr int32 IsFullNameStableForNetworking = 58;
        constexpr int32 IsSupportedForNetworking = 59;
        constexpr int32 GetSubobjectsWithStableNamesForNetworking = 60;
        constexpr int32 PreNetReceive = 61;
        constexpr int32 PostNetReceive = 62;
        constexpr int32 PostRepNotifies = 63;
        constexpr int32 PreDestroyFromReplication = 64;
        constexpr int32 BuildSubobjectMapping = 65;
        constexpr int32 GetConfigOverridePlatform = 66;
        constexpr int32 OverridePerObjectConfigSection = 67;
        constexpr int32 ProcessEvent = 68;
        constexpr int32 GetFunctionCallspace = 69;
        constexpr int32 CallRemoteFunction = 70;
        constexpr int32 ProcessConsoleExec = 71;
        constexpr int32 RegenerateClass = 72;
        constexpr int32 MarkAsEditorOnlySubobject = 73;
        constexpr int32 CheckDefaultSubobjectsInternal = 74;
        constexpr int32 ValidateGeneratedRepEnums = 75;
        constexpr int32 SetNetPushIdDynamic = 76;
        constexpr int32 GetNetPushIdDynamic = 77;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Per-class slot aliases (use these at call sites for readability)
    // ─────────────────────────────────────────────────────────────────────────
    namespace UObjectBase
    {
        constexpr int32 VecDelDtor = Slots::VecDelDtor;
        constexpr int32 RegisterDependencies = Slots::RegisterDependencies;
        constexpr int32 DeferredRegister = Slots::DeferredRegister;
    }
    namespace UObjectBaseUtility
    {
        constexpr int32 VecDelDtor = Slots::VecDelDtor;
        constexpr int32 CanBeClusterRoot = Slots::CanBeClusterRoot;
        constexpr int32 CanBeInCluster = Slots::CanBeInCluster;
        constexpr int32 CreateCluster = Slots::CreateCluster;
        constexpr int32 OnClusterMarkedAsPendingKill = Slots::OnClusterMarkedAsPendingKill;
    }
    namespace UObject
    {
        constexpr int32 VecDelDtor = Slots::VecDelDtor;
        constexpr int32 GetDetailedInfoInternal = Slots::GetDetailedInfoInternal;
        constexpr int32 PostInitProperties = Slots::PostInitProperties;
        constexpr int32 PostCDOContruct = Slots::PostCDOContruct;
        constexpr int32 PreSaveRoot = Slots::PreSaveRoot;
        constexpr int32 PostSaveRoot = Slots::PostSaveRoot;
        constexpr int32 PreSave = Slots::PreSave;
        constexpr int32 IsReadyForAsyncPostLoad = Slots::IsReadyForAsyncPostLoad;
        constexpr int32 PostLoad = Slots::PostLoad;
        constexpr int32 PostLoadSubobjects = Slots::PostLoadSubobjects;
        constexpr int32 BeginDestroy = Slots::BeginDestroy;
        constexpr int32 IsReadyForFinishDestroy = Slots::IsReadyForFinishDestroy;
        constexpr int32 FinishDestroy = Slots::FinishDestroy;
        constexpr int32 Serialize_Archive = Slots::Serialize_Archive;
        constexpr int32 Serialize_Structured = Slots::Serialize_Structured;
        constexpr int32 ShutdownAfterError = Slots::ShutdownAfterError;
        constexpr int32 PostInterpChange = Slots::PostInterpChange;
        constexpr int32 PostRename = Slots::PostRename;
        constexpr int32 PreDuplicate = Slots::PreDuplicate;
        constexpr int32 PostDuplicate_Mode = Slots::PostDuplicate_Mode;
        constexpr int32 PostDuplicate_Bool = Slots::PostDuplicate_Bool;
        constexpr int32 NeedsLoadForClient = Slots::NeedsLoadForClient;
        constexpr int32 NeedsLoadForServer = Slots::NeedsLoadForServer;
        constexpr int32 NeedsLoadForTargetPlatform = Slots::NeedsLoadForTargetPlatform;
        constexpr int32 NeedsLoadForEditorGame = Slots::NeedsLoadForEditorGame;
        constexpr int32 IsEditorOnly = Slots::IsEditorOnly;
        constexpr int32 HasNonEditorOnlyReferences = Slots::HasNonEditorOnlyReferences;
        constexpr int32 IsPostLoadThreadSafe = Slots::IsPostLoadThreadSafe;
        constexpr int32 IsDestructionThreadSafe = Slots::IsDestructionThreadSafe;
        constexpr int32 GetPreloadDependencies = Slots::GetPreloadDependencies;
        constexpr int32 GetPrestreamPackages = Slots::GetPrestreamPackages;
        constexpr int32 ExportCustomProperties = Slots::ExportCustomProperties;
        constexpr int32 ImportCustomProperties = Slots::ImportCustomProperties;
        constexpr int32 PostEditImport = Slots::PostEditImport;
        constexpr int32 PostReloadConfig = Slots::PostReloadConfig;
        constexpr int32 Rename = Slots::Rename;
        constexpr int32 GetDesc = Slots::GetDesc;
        constexpr int32 GetSparseClassDataStruct = Slots::GetSparseClassDataStruct;
        constexpr int32 GetWorld = Slots::GetWorld;
        constexpr int32 GetNativePropertyValues = Slots::GetNativePropertyValues;
        constexpr int32 GetResourceSizeEx = Slots::GetResourceSizeEx;
        constexpr int32 GetExporterName = Slots::GetExporterName;
        constexpr int32 GetRestoreForUObjectOverwrite = Slots::GetRestoreForUObjectOverwrite;
        constexpr int32 AreNativePropertiesIdenticalTo = Slots::AreNativePropertiesIdenticalTo;
        constexpr int32 GetAssetRegistryTags = Slots::GetAssetRegistryTags;
        constexpr int32 IsAsset = Slots::IsAsset;
        constexpr int32 GetPrimaryAssetId = Slots::GetPrimaryAssetId;
        constexpr int32 IsLocalizedResource = Slots::IsLocalizedResource;
        constexpr int32 IsSafeForRootSet = Slots::IsSafeForRootSet;
        constexpr int32 TagSubobjects = Slots::TagSubobjects;
        constexpr int32 GetLifetimeReplicatedProps = Slots::GetLifetimeReplicatedProps;
        constexpr int32 IsNameStableForNetworking = Slots::IsNameStableForNetworking;
        constexpr int32 IsFullNameStableForNetworking = Slots::IsFullNameStableForNetworking;
        constexpr int32 IsSupportedForNetworking = Slots::IsSupportedForNetworking;
        constexpr int32 GetSubobjectsWithStableNamesForNetworking = Slots::GetSubobjectsWithStableNamesForNetworking;
        constexpr int32 PreNetReceive = Slots::PreNetReceive;
        constexpr int32 PostNetReceive = Slots::PostNetReceive;
        constexpr int32 PostRepNotifies = Slots::PostRepNotifies;
        constexpr int32 PreDestroyFromReplication = Slots::PreDestroyFromReplication;
        constexpr int32 BuildSubobjectMapping = Slots::BuildSubobjectMapping;
        constexpr int32 GetConfigOverridePlatform = Slots::GetConfigOverridePlatform;
        constexpr int32 OverridePerObjectConfigSection = Slots::OverridePerObjectConfigSection;
        constexpr int32 ProcessEvent = Slots::ProcessEvent;
        constexpr int32 GetFunctionCallspace = Slots::GetFunctionCallspace;
        constexpr int32 CallRemoteFunction = Slots::CallRemoteFunction;
        constexpr int32 ProcessConsoleExec = Slots::ProcessConsoleExec;
        constexpr int32 RegenerateClass = Slots::RegenerateClass;
        constexpr int32 MarkAsEditorOnlySubobject = Slots::MarkAsEditorOnlySubobject;
        constexpr int32 CheckDefaultSubobjectsInternal = Slots::CheckDefaultSubobjectsInternal;
        constexpr int32 ValidateGeneratedRepEnums = Slots::ValidateGeneratedRepEnums;
        constexpr int32 SetNetPushIdDynamic = Slots::SetNetPushIdDynamic;
        constexpr int32 GetNetPushIdDynamic = Slots::GetNetPushIdDynamic;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  UObjectVCalls
//  Three access patterns per function:
//    ::GetPtr(Obj)        — raw function pointer from vtable, for hooking
//    ::Call(Obj, ...)     — invoke directly
//    Signature type lives in VTableLayout::Signatures::F<Name>
// ─────────────────────────────────────────────────────────────────────────────
namespace UObjectVCalls
{
    using namespace UC;
    using SDK::UObject;
    using SDK::FName;
    using UC::FString;

    namespace Detail
    {
        template<typename TSig>
        FORCEINLINE TSig* GetFn(const void* Obj, int32 Slot)
        {
            return InSDKUtils::GetVirtualFunction<TSig*>(Obj, Slot);
        }
    }

    // ── UObjectBase ───────────────────────────────────────────────────────────

    struct VecDelDtor
    {
        using Sig = VTableLayout::Signatures::FVecDelDtor;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::VecDelDtor); }
        static FORCEINLINE void* Call(UObject* Obj, uint32 Flags = 0) { return GetPtr(Obj)(Obj, Flags); }
    };

    struct RegisterDependencies
    {
        using Sig = VTableLayout::Signatures::FRegisterDependencies;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::RegisterDependencies); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct DeferredRegister
    {
        using Sig = VTableLayout::Signatures::FDeferredRegister;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::DeferredRegister); }
        static FORCEINLINE void Call(UObject* Obj, SDK::UClass* UClassStaticClass, const TCHAR* PackageName, const TCHAR* Name) { GetPtr(Obj)(Obj, UClassStaticClass, PackageName, Name); }
    };

    // ── UObjectBaseUtility ────────────────────────────────────────────────────

    struct CanBeClusterRoot
    {
        using Sig = VTableLayout::Signatures::FCanBeClusterRoot;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::CanBeClusterRoot); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct CanBeInCluster
    {
        using Sig = VTableLayout::Signatures::FCanBeInCluster;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::CanBeInCluster); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct CreateCluster
    {
        using Sig = VTableLayout::Signatures::FCreateCluster;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::CreateCluster); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct OnClusterMarkedAsPendingKill
    {
        using Sig = VTableLayout::Signatures::FOnClusterMarkedAsPendingKill;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::OnClusterMarkedAsPendingKill); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    // ── UObject ───────────────────────────────────────────────────────────────

    struct GetDetailedInfoInternal
    {
        using Sig = VTableLayout::Signatures::FGetDetailedInfoInternal;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetDetailedInfoInternal); }
        static FORCEINLINE FString Call(UObject* Obj) { FString Out; GetPtr(Obj)(Obj, &Out); return Out; }
    };

    struct PostInitProperties
    {
        using Sig = VTableLayout::Signatures::FPostInitProperties;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostInitProperties); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct PostCDOContruct
    {
        using Sig = VTableLayout::Signatures::FPostCDOContruct;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostCDOContruct); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct PreSaveRoot
    {
        using Sig = VTableLayout::Signatures::FPreSaveRoot;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PreSaveRoot); }
        static FORCEINLINE bool Call(UObject* Obj, const TCHAR* Filename) { return GetPtr(Obj)(Obj, Filename); }
    };

    struct PostSaveRoot
    {
        using Sig = VTableLayout::Signatures::FPostSaveRoot;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostSaveRoot); }
        static FORCEINLINE void Call(UObject* Obj, bool bCleanupIsRequired) { GetPtr(Obj)(Obj, bCleanupIsRequired); }
    };

    struct PreSave
    {
        using Sig = VTableLayout::Signatures::FPreSave;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PreSave); }
        static FORCEINLINE void Call(UObject* Obj, const void* /*ITargetPlatform*/ TP) { GetPtr(Obj)(Obj, TP); }
    };

    struct IsReadyForAsyncPostLoad
    {
        using Sig = VTableLayout::Signatures::FIsReadyForAsyncPostLoad;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::IsReadyForAsyncPostLoad); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct PostLoad
    {
        using Sig = VTableLayout::Signatures::FPostLoad;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostLoad); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct PostLoadSubobjects
    {
        using Sig = VTableLayout::Signatures::FPostLoadSubobjects;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostLoadSubobjects); }
        static FORCEINLINE void Call(UObject* Obj, void* /*FObjectInstancingGraph**/ G) { GetPtr(Obj)(Obj, G); }
    };

    struct BeginDestroy
    {
        using Sig = VTableLayout::Signatures::FBeginDestroy;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::BeginDestroy); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct IsReadyForFinishDestroy
    {
        using Sig = VTableLayout::Signatures::FIsReadyForFinishDestroy;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::IsReadyForFinishDestroy); }
        static FORCEINLINE bool Call(UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct FinishDestroy
    {
        using Sig = VTableLayout::Signatures::FFinishDestroy;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::FinishDestroy); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct Serialize_Archive
    {
        using Sig = VTableLayout::Signatures::FSerialize_Archive;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::Serialize_Archive); }
        static FORCEINLINE void Call(UObject* Obj, void* /*FArchive&*/ Ar) { GetPtr(Obj)(Obj, Ar); }
    };

    struct Serialize_Structured
    {
        using Sig = VTableLayout::Signatures::FSerialize_Structured;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::Serialize_Structured); }
        static FORCEINLINE void Call(UObject* Obj, void* /*FStructuredArchive::FRecord*/ R) { GetPtr(Obj)(Obj, R); }
    };

    struct ShutdownAfterError
    {
        using Sig = VTableLayout::Signatures::FShutdownAfterError;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::ShutdownAfterError); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct PostInterpChange
    {
        using Sig = VTableLayout::Signatures::FPostInterpChange;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostInterpChange); }
        static FORCEINLINE void Call(UObject* Obj, SDK::FProperty* Prop) { GetPtr(Obj)(Obj, Prop); }
    };

    // ── UObject (continued) ───────────────────────────────────────────────────

    struct PostRename
    {
        using Sig = VTableLayout::Signatures::FPostRename;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostRename); }
        static FORCEINLINE void Call(UObject* Obj, UObject* OldOuter, const FName OldName) { GetPtr(Obj)(Obj, OldOuter, OldName); }
    };

    struct PreDuplicate
    {
        using Sig = VTableLayout::Signatures::FPreDuplicate;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PreDuplicate); }
        static FORCEINLINE void Call(UObject* Obj, void* /*FObjectDuplicationParameters&*/ P) { GetPtr(Obj)(Obj, P); }
    };

    // PostDuplicate_Mode has no signature defined (EDuplicateMode::Type unavailable) —
    // GetPtr only, caller casts manually if needed.
    struct PostDuplicate_Mode
    {
        static FORCEINLINE void* GetPtr(const UObject* Obj)
        {
            void** VTable = *reinterpret_cast<void***>(const_cast<UObject*>(Obj));
            return VTable[VTableLayout::Slots::PostDuplicate_Mode];
        }
    };

    struct PostDuplicate_Bool
    {
        using Sig = VTableLayout::Signatures::FPostDuplicate_Bool;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostDuplicate_Bool); }
        static FORCEINLINE void Call(UObject* Obj, bool bDuplicateForPIE) { GetPtr(Obj)(Obj, bDuplicateForPIE); }
    };

    struct NeedsLoadForClient
    {
        using Sig = VTableLayout::Signatures::FNeedsLoadForClient;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::NeedsLoadForClient); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct NeedsLoadForServer
    {
        using Sig = VTableLayout::Signatures::FNeedsLoadForServer;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::NeedsLoadForServer); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct NeedsLoadForTargetPlatform
    {
        using Sig = VTableLayout::Signatures::FNeedsLoadForTargetPlatform;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::NeedsLoadForTargetPlatform); }
        static FORCEINLINE bool Call(const UObject* Obj, const void* /*ITargetPlatform*/ TP) { return GetPtr(Obj)(Obj, TP); }
    };

    struct NeedsLoadForEditorGame
    {
        using Sig = VTableLayout::Signatures::FNeedsLoadForEditorGame;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::NeedsLoadForEditorGame); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct IsEditorOnly
    {
        using Sig = VTableLayout::Signatures::FIsEditorOnly;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::IsEditorOnly); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct HasNonEditorOnlyReferences
    {
        using Sig = VTableLayout::Signatures::FHasNonEditorOnlyReferences;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::HasNonEditorOnlyReferences); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct IsPostLoadThreadSafe
    {
        using Sig = VTableLayout::Signatures::FIsPostLoadThreadSafe;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::IsPostLoadThreadSafe); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct IsDestructionThreadSafe
    {
        using Sig = VTableLayout::Signatures::FIsDestructionThreadSafe;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::IsDestructionThreadSafe); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct GetPreloadDependencies
    {
        using Sig = VTableLayout::Signatures::FGetPreloadDependencies;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetPreloadDependencies); }
        static FORCEINLINE void Call(UObject* Obj, TArray<UObject*>& Out) { GetPtr(Obj)(Obj, Out); }
    };

    struct GetPrestreamPackages
    {
        using Sig = VTableLayout::Signatures::FGetPrestreamPackages;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetPrestreamPackages); }
        static FORCEINLINE void Call(UObject* Obj, TArray<UObject*>& Out) { GetPtr(Obj)(Obj, Out); }
    };

    struct ExportCustomProperties
    {
        using Sig = VTableLayout::Signatures::FExportCustomProperties;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::ExportCustomProperties); }
        static FORCEINLINE void Call(UObject* Obj, void* /*FOutputDevice&*/ Out, uint32 Indent) { GetPtr(Obj)(Obj, Out, Indent); }
    };

    struct ImportCustomProperties
    {
        using Sig = VTableLayout::Signatures::FImportCustomProperties;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::ImportCustomProperties); }
        static FORCEINLINE void Call(UObject* Obj, const TCHAR* Src, void* /*FFeedbackContext**/ Warn) { GetPtr(Obj)(Obj, Src, Warn); }
    };

    struct PostEditImport
    {
        using Sig = VTableLayout::Signatures::FPostEditImport;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostEditImport); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct PostReloadConfig
    {
        using Sig = VTableLayout::Signatures::FPostReloadConfig;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostReloadConfig); }
        static FORCEINLINE void Call(UObject* Obj, SDK::FProperty* Prop) { GetPtr(Obj)(Obj, Prop); }
    };

    struct Rename
    {
        using Sig = VTableLayout::Signatures::FRename;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::Rename); }
        static FORCEINLINE bool Call(UObject* Obj, const TCHAR* NewName, UObject* NewOuter, ERenameFlags Flags = REN::None) { return GetPtr(Obj)(Obj, NewName, NewOuter, Flags); }
    };

    struct GetDesc
    {
        using Sig = VTableLayout::Signatures::FGetDesc;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetDesc); }
        static FORCEINLINE FString Call(UObject* Obj) { FString Out; GetPtr(Obj)(Obj, &Out); return Out; }
    };

    struct GetSparseClassDataStruct
    {
        using Sig = VTableLayout::Signatures::FGetSparseClassDataStruct;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetSparseClassDataStruct); }
        static FORCEINLINE UScriptStruct* Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct GetWorld
    {
        using Sig = VTableLayout::Signatures::FGetWorld;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetWorld); }
        static FORCEINLINE UWorld* Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct GetNativePropertyValues
    {
        using Sig = VTableLayout::Signatures::FGetNativePropertyValues;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetNativePropertyValues); }
        static FORCEINLINE bool Call(const UObject* Obj, TMap<FString, FString>& Out, uint32 Flags = 0) { return GetPtr(Obj)(Obj, Out, Flags); }
    };

    struct GetResourceSizeEx
    {
        using Sig = VTableLayout::Signatures::FGetResourceSizeEx;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetResourceSizeEx); }
        static FORCEINLINE void Call(UObject* Obj, void* /*FResourceSizeEx&*/ Out) { GetPtr(Obj)(Obj, Out); }
    };

    struct GetExporterName
    {
        using Sig = VTableLayout::Signatures::FGetExporterName;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetExporterName); }
        static FORCEINLINE FName Call(UObject* Obj) { FName Out; GetPtr(Obj)(Obj, &Out); return Out; }
    };

    // GetRestoreForUObjectOverwrite has no signature defined (FRestoreForUObjectOverwrite unavailable) —
    // GetPtr only, caller casts manually if needed.
    struct GetRestoreForUObjectOverwrite
    {
        static FORCEINLINE void* GetPtr(const UObject* Obj)
        {
            void** VTable = *reinterpret_cast<void***>(const_cast<UObject*>(Obj));
            return VTable[VTableLayout::Slots::GetRestoreForUObjectOverwrite];
        }
    };

    struct AreNativePropertiesIdenticalTo
    {
        using Sig = VTableLayout::Signatures::FAreNativePropertiesIdenticalTo;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::AreNativePropertiesIdenticalTo); }
        static FORCEINLINE bool Call(const UObject* Obj, UObject* Other) { return GetPtr(Obj)(Obj, Other); }
    };

    struct GetAssetRegistryTags
    {
        using Sig = VTableLayout::Signatures::FGetAssetRegistryTags;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetAssetRegistryTags); }
        static FORCEINLINE void Call(const UObject* Obj, void* /*TArray<UObject::FAssetRegistryTag>&*/ Out) { GetPtr(Obj)(Obj, Out); }
    };

    struct IsAsset
    {
        using Sig = VTableLayout::Signatures::FIsAsset;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::IsAsset); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct GetPrimaryAssetId
    {
        using Sig = VTableLayout::Signatures::FGetPrimaryAssetId;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetPrimaryAssetId); }
        static FORCEINLINE FPrimaryAssetId Call(const UObject* Obj) { FPrimaryAssetId Out; GetPtr(Obj)(Obj, &Out); return Out; }
    };

    struct IsLocalizedResource
    {
        using Sig = VTableLayout::Signatures::FIsLocalizedResource;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::IsLocalizedResource); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct IsSafeForRootSet
    {
        using Sig = VTableLayout::Signatures::FIsSafeForRootSet;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::IsSafeForRootSet); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct TagSubobjects
    {
        using Sig = VTableLayout::Signatures::FTagSubobjects;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::TagSubobjects); }
        static FORCEINLINE void Call(UObject* Obj, EObjectFlags Flags) { GetPtr(Obj)(Obj, Flags); }
    };

    struct GetLifetimeReplicatedProps
    {
        using Sig = VTableLayout::Signatures::FGetLifetimeReplicatedProps;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetLifetimeReplicatedProps); }
        static FORCEINLINE void Call(const UObject* Obj, TArray<FLifetimeProperty>& Out) { GetPtr(Obj)(Obj, Out); }
    };

    struct IsNameStableForNetworking
    {
        using Sig = VTableLayout::Signatures::FIsNameStableForNetworking;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::IsNameStableForNetworking); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct IsFullNameStableForNetworking
    {
        using Sig = VTableLayout::Signatures::FIsFullNameStableForNetworking;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::IsFullNameStableForNetworking); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct IsSupportedForNetworking
    {
        using Sig = VTableLayout::Signatures::FIsSupportedForNetworking;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::IsSupportedForNetworking); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct GetSubobjectsWithStableNamesForNetworking
    {
        using Sig = VTableLayout::Signatures::FGetSubobjectsWithStableNamesForNetworking;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetSubobjectsWithStableNamesForNetworking); }
        static FORCEINLINE void Call(UObject* Obj, TArray<UObject*>& Out) { GetPtr(Obj)(Obj, Out); }
    };

    struct PreNetReceive
    {
        using Sig = VTableLayout::Signatures::FPreNetReceive;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PreNetReceive); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct PostNetReceive
    {
        using Sig = VTableLayout::Signatures::FPostNetReceive;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostNetReceive); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct PostRepNotifies
    {
        using Sig = VTableLayout::Signatures::FPostRepNotifies;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PostRepNotifies); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct PreDestroyFromReplication
    {
        using Sig = VTableLayout::Signatures::FPreDestroyFromReplication;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::PreDestroyFromReplication); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct BuildSubobjectMapping
    {
        using Sig = VTableLayout::Signatures::FBuildSubobjectMapping;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::BuildSubobjectMapping); }
        static FORCEINLINE void Call(const UObject* Obj, UObject* Other, TMap<UObject*, UObject*>& Map) { GetPtr(Obj)(Obj, Other, Map); }
    };

    struct GetConfigOverridePlatform
    {
        using Sig = VTableLayout::Signatures::FGetConfigOverridePlatform;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetConfigOverridePlatform); }
        static FORCEINLINE const TCHAR* Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct OverridePerObjectConfigSection
    {
        using Sig = VTableLayout::Signatures::FOverridePerObjectConfigSection;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::OverridePerObjectConfigSection); }
        static FORCEINLINE void Call(UObject* Obj, FString& Section) { GetPtr(Obj)(Obj, Section); }
    };

    struct ProcessEvent
    {
        using Sig = VTableLayout::Signatures::FProcessEvent;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::ProcessEvent); }
        static FORCEINLINE void Call(UObject* Obj, UFunction* Fn, void* Parms) { GetPtr(Obj)(Obj, Fn, Parms); }
    };

    struct GetFunctionCallspace
    {
        using Sig = VTableLayout::Signatures::FGetFunctionCallspace;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetFunctionCallspace); }
        static FORCEINLINE int32 Call(UObject* Obj, UFunction* Fn, void* /*FFrame**/ Stack) { return GetPtr(Obj)(Obj, Fn, Stack); }
    };

    struct CallRemoteFunction
    {
        using Sig = VTableLayout::Signatures::FCallRemoteFunction;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::CallRemoteFunction); }
        static FORCEINLINE bool Call(UObject* Obj, UFunction* Fn, void* Parms, void* /*FOutParmRec**/ Out, void* /*FFrame**/ Stack) { return GetPtr(Obj)(Obj, Fn, Parms, Out, Stack); }
    };

    struct ProcessConsoleExec
    {
        using Sig = VTableLayout::Signatures::FProcessConsoleExec;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::ProcessConsoleExec); }
        static FORCEINLINE bool Call(UObject* Obj, const TCHAR* Cmd, void* /*FOutputDevice&*/ Ar, UObject* Exec) { return GetPtr(Obj)(Obj, Cmd, Ar, Exec); }
    };

    struct RegenerateClass
    {
        using Sig = VTableLayout::Signatures::FRegenerateClass;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::RegenerateClass); }
        static FORCEINLINE UClass* Call(UObject* Obj, UClass* ClassToRegen, UObject* PreviousCDO) { return GetPtr(Obj)(Obj, ClassToRegen, PreviousCDO); }
    };

    struct MarkAsEditorOnlySubobject
    {
        using Sig = VTableLayout::Signatures::FMarkAsEditorOnlySubobject;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::MarkAsEditorOnlySubobject); }
        static FORCEINLINE void Call(UObject* Obj) { GetPtr(Obj)(Obj); }
    };

    struct CheckDefaultSubobjectsInternal
    {
        using Sig = VTableLayout::Signatures::FCheckDefaultSubobjectsInternal;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::CheckDefaultSubobjectsInternal); }
        static FORCEINLINE bool Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };

    struct ValidateGeneratedRepEnums
    {
        using Sig = VTableLayout::Signatures::FValidateGeneratedRepEnums;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::ValidateGeneratedRepEnums); }
        static FORCEINLINE void Call(const UObject* Obj, const void* /*TArray<FRepRecord>&*/ Recs) { GetPtr(Obj)(Obj, Recs); }
    };

    struct SetNetPushIdDynamic
    {
        using Sig = VTableLayout::Signatures::FSetNetPushIdDynamic;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::SetNetPushIdDynamic); }
        static FORCEINLINE void Call(UObject* Obj, int32 NewId) { GetPtr(Obj)(Obj, NewId); }
    };

    struct GetNetPushIdDynamic
    {
        using Sig = VTableLayout::Signatures::FGetNetPushIdDynamic;
        static FORCEINLINE Sig* GetPtr(const UObject* Obj) { return Detail::GetFn<Sig>(Obj, VTableLayout::Slots::GetNetPushIdDynamic); }
        static FORCEINLINE int32 Call(const UObject* Obj) { return GetPtr(Obj)(Obj); }
    };
}