#pragma once
// Lib_PropertyAccess.h — Property memory access helpers and GetTypeName.

#ifdef RogueCore // Im so done with intellisense failing to deduce types that are sdk dependant
#include "../../RcMods/Lib_Forward.h"
#else
#include "../../DrgMods/Lib_Forward.h"
#endif

template<typename T> T* GetPropertyPtr(uintptr_t Base, int32 Offset) { return reinterpret_cast<T*>(Base + Offset); }
template<typename T> T* GetPropertyPtr(SDK::UObject* Base, int32 Offset) { return GetPropertyPtr<T>(reinterpret_cast<uintptr_t>(Base), Offset); }
template<typename T> T& GetPropertyRef(SDK::UObject* Base, int32 Offset) { return *GetPropertyPtr<T>(Base, Offset); }
template<typename T> T  GetPropertyValue(SDK::UObject* Base, int32 Offset) { return *GetPropertyPtr<T>(Base, Offset); }

bool        ReadBool    (SDK::UObject* Object, SDK::FBoolProperty* Prop);
void        WriteBool   (SDK::UObject* Object, SDK::FBoolProperty* Prop, bool Value);
std::string GetTypeName (SDK::FField* field);
