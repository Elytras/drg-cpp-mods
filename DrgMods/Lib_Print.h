#pragma once
// Lib_Print.h — PrintFieldValue, DumpItemProperties, BuildClassChain, Detail.

#include <string>
#include <vector>
#include "Lib_Forward.h"

// =========================================================================
// Class chain helpers
// =========================================================================

std::vector<UStruct*> BuildClassChain(UClass* Class, UClass* OuterBase = nullptr);
void                  PrintClassHierarchy(const UClass* Class);

// =========================================================================
// Detail namespace
// =========================================================================

namespace Detail
{
    std::string Prefix     (const std::vector<bool>& ancestry, bool isLast);
    std::string ChildPrefix(const std::vector<bool>& ancestry, bool open);
    std::string ClassTag   (const UClass* cls);
} // namespace Detail

// =========================================================================
// PrintFieldValue / GetFieldValueAsString
// =========================================================================

void        PrintFieldValue      (uintptr_t Base, FField* Field, const std::vector<bool>& ancestry, bool isLast, const std::string& labelOverride = "");
void        PrintFieldValue      (UObject*  Base, FField* Field, const std::vector<bool>& ancestry, bool isLast, const std::string& labelOverride = "");
std::string GetFieldValueAsString(uintptr_t Base, FField* Field);

// =========================================================================
// DumpItemProperties
// =========================================================================

template <bool bSort = false>
void DumpItemProperties(UObject* Item, UClass* OuterBase = nullptr);

extern template void DumpItemProperties<false>(UObject*, UClass*);
extern template void DumpItemProperties<true> (UObject*, UClass*);

// =========================================================================
// DumpItemPropertiesSorted
// =========================================================================

void DumpItemPropertiesSorted(UObject* Item, UClass* OuterBase = nullptr);

// =========================================================================
// Flag diagnostics
// =========================================================================

std::string DumpFunctionFlags(uint32_t flags);
std::string DumpPropertyFlags(SDK::EPropertyFlags flags);
std::string DumpPropertyFlags(uint64_t flags);
