#pragma once
// Lib_VTableInfo.h — RogueCore (UE 5.6).
//
// Intentionally minimal. The DrgMods/Lib_VTableInfo.h equivalent is FULLY
// UE 4.27-specific: every constant in its `VTableLayout::Slots` namespace
// (UObjectBase 0-2, UObjectBaseUtility 3-6, UObject 7-77) was derived from
// the FSD-Win64-Shipping.exe vtable, and several method signatures use UE 4.27
// conventions (e.g. `OnClusterMarkedAsPendingKill`, the `PostDuplicate(EDuplicateMode)`
// vs `PostDuplicate(bool)` split, `GetRestoreForUObjectOverwrite`, etc.).
//
// Porting any of those to UE 5.6 requires a fresh vtable dump from
// RogueCore-Win64-Shipping.exe — slot order changed, some methods were removed,
// and several gained new arguments (e.g. GetAssetRegistryTags now takes
// FAssetRegistryTagsContext, PreSave takes FObjectPreSaveContext, etc.).
//
// We do NOT redeclare `Offsets::ProcessEventIdx` here — Dumper-7 already
// generates it as `SDK::Offsets::ProcessEventIdx` in `SDK/SDK/Basic.hpp`
// (0x4C / 76 for RogueCore). Lib_GameHooks.cpp picks it up through its
// `using namespace SDK;` directive. Adding a global `::Offsets::ProcessEventIdx`
// here would cause an ambiguous-lookup error in Lib_GameHooks.cpp.
//
// When porting `VTableLayout` for RC:
//   1. Dump the UObject vtable from a running RC-Win64-Shipping.exe.
//   2. Cross-reference each slot with Engine/Source/Runtime/CoreUObject/Public/UObject/Object.h
//      (or the UE5 source on GitHub) to identify method names + signatures.
//   3. Re-create the `VTableLayout::Slots` and `VTableLayout::Signatures` namespaces
//      with the RC numbers/types. Several UE4 entries will be gone; new ones added.
//   4. Re-enable the `HOOKALIASES`-gated `Hooks::` namespace in Lib_VTableHook.h
//      once the slot constants exist, OR just call SlotHook<N, FSig> directly
//      with raw slot numbers at the call sites.

// Minimal VTableLayout scaffolding — only exposes the UEngine::Tick slot used
// by GameHooks::EngineTickHook. Counted from SDK/VTableLayout_5_06_Template.ini:
//   UObjectBase           slots 0-3   (4 entries inc. __vecDelDtor)
//   UObjectBaseUtility    slots 4-8   (+5 new virtuals)
//   UObject               slots 9-85  (+77 new virtuals)
//   UEngine               slots 86+   (__vecDelDtor override at slot 0, then
//                                      WorldAdded@86, WorldDestroyed@87, …, Tick@94)
namespace VTableLayout
{
    namespace Slots
    {
        constexpr int32 VecDelDtor  = 0;
        constexpr int32 Engine_Tick = 94;
    }
    namespace UEngine
    {
        constexpr int32 VecDelDtor = Slots::VecDelDtor;
        constexpr int32 Tick       = Slots::Engine_Tick;
    }
}
