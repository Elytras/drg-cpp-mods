#pragma once
// Lib_Print.h — PrintFieldValue, DumpItemProperties, BuildClassChain, Detail.

#include <string>
#include <vector>
#include "Lib_Forward.h"

// =========================================================================
// Class chain helpers
// =========================================================================

std::vector<SDK::UStruct*> BuildClassChain(SDK::UClass* Class, SDK::UClass* OuterBase = nullptr);
void                       PrintClassHierarchy(const SDK::UClass* Class);

// =========================================================================
// Detail namespace
// =========================================================================

namespace Detail
{
    std::string Prefix     (const std::vector<bool>& ancestry, bool isLast);
    std::string ChildPrefix(const std::vector<bool>& ancestry, bool open);
    std::string ClassTag   (const SDK::UClass* cls);
} // namespace Detail

// =========================================================================
// PrintFieldValue / GetFieldValueAsString
// =========================================================================

void        PrintFieldValue      (uintptr_t Base, SDK::FField* Field, const std::vector<bool>& ancestry, bool isLast, const std::string& labelOverride = "");
void        PrintFieldValue      (SDK::UObject* Base, SDK::FField* Field, const std::vector<bool>& ancestry, bool isLast, const std::string& labelOverride = "");
std::string GetFieldValueAsString(uintptr_t Base, SDK::FField* Field);

// =========================================================================
// DumpItemProperties
// =========================================================================

template <bool bSort = false>
void DumpItemProperties(SDK::UObject* Item, SDK::UClass* OuterBase = nullptr);

extern template void DumpItemProperties<false>(SDK::UObject*, SDK::UClass*);
extern template void DumpItemProperties<true> (SDK::UObject*, SDK::UClass*);

// =========================================================================
// DumpItemPropertiesSorted
// =========================================================================

void DumpItemPropertiesSorted(SDK::UObject* Item, SDK::UClass* OuterBase = nullptr);

// =========================================================================
// Flag diagnostics
// =========================================================================

std::string DumpFunctionFlags(uint32_t flags);
std::string DumpPropertyFlags(SDK::EPropertyFlags flags);
std::string DumpPropertyFlags(uint64_t flags);
