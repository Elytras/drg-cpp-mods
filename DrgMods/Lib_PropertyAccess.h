#pragma once
// Lib_PropertyAccess.h � Property memory access helpers and GetTypeName.

#include "Lib_Forward.h"

template<typename T> T* GetPropertyPtr(uintptr_t Base, int32 Offset) { return reinterpret_cast<T*>(Base + Offset); }
template<typename T> T* GetPropertyPtr(UObject* Base, int32 Offset) { return GetPropertyPtr<T>(reinterpret_cast<uintptr_t>(Base), Offset); }
template<typename T> T& GetPropertyRef(UObject* Base, int32 Offset) { return *GetPropertyPtr<T>(Base, Offset); }
template<typename T> T  GetPropertyValue(UObject* Base, int32 Offset) { return *GetPropertyPtr<T>(Base, Offset); }

bool        ReadBool    (UObject* Object, FBoolProperty* Prop);
void        WriteBool   (UObject* Object, FBoolProperty* Prop, bool Value);
std::string GetTypeName (FField* field);