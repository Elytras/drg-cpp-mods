#pragma once
// Lib_PropertyAccess.h — Property memory access helpers and GetTypeName.

#include "Lib_Forward.h"
#include "Lib_ObjectCast.h"

template<typename T> T* GetPropertyPtr(uintptr_t Base, int32 Offset) { return reinterpret_cast<T*>(Base + Offset); }
template<typename T> T* GetPropertyPtr(UObject* Base, int32 Offset) { return GetPropertyPtr<T>(reinterpret_cast<uintptr_t>(Base), Offset); }
template<typename T> T& GetPropertyRef(UObject* Base, int32 Offset) { return *GetPropertyPtr<T>(Base, Offset); }
template<typename T> T  GetPropertyValue(UObject* Base, int32 Offset) { return *GetPropertyPtr<T>(Base, Offset); }

inline bool ReadBool(UObject* Object, FBoolProperty* Prop)
{
    if (Prop->FieldMask == 0xFF)
        return *GetPropertyPtr<bool>(Object, Prop->Offset);
    const uint8* byte = reinterpret_cast<const uint8*>(
        reinterpret_cast<uintptr_t>(Object) + Prop->Offset + Prop->ByteOffset);
    return (*byte & Prop->FieldMask) != 0;
}

inline void WriteBool(UObject* Object, FBoolProperty* Prop, bool Value)
{
    if (Prop->FieldMask == 0xFF) { *GetPropertyPtr<bool>(Object, Prop->Offset) = Value; return; }
    uint8* byte = reinterpret_cast<uint8*>(
        reinterpret_cast<uintptr_t>(Object) + Prop->Offset + Prop->ByteOffset);
    if (Value) *byte |= Prop->FieldMask;
    else       *byte &= ~Prop->FieldMask;
}

inline std::string GetTypeName(FField* field)
{
    std::string result = "?";
    FieldCast::Visit(field, [&](auto* prop)
        {
            using T = std::remove_pointer_t<decltype(prop)>;
            if constexpr (std::is_same_v<T, FStructProperty>)
                result = prop->Struct ? prop->Struct->GetName() : "struct";
            else if constexpr (std::is_same_v<T, FClassProperty>)
                result = prop->PropertyClass ? "TSubclassOf<" + prop->PropertyClass->GetName() + ">" : "UClass";
            else if constexpr (std::is_same_v<T, FSoftClassProperty>)
                result = prop->PropertyClass ? "TSoftClassPtr<" + prop->PropertyClass->GetName() + ">" : "TSoftClassPtr";
            else if constexpr (std::is_same_v<T, FSoftObjectProperty>)
                result = prop->PropertyClass ? "TSoftObjectPtr<" + prop->PropertyClass->GetName() + ">" : "TSoftObjectPtr";
            else if constexpr (std::is_same_v<T, FWeakObjectProperty>)
                result = prop->PropertyClass ? "TWeakObjectPtr<" + prop->PropertyClass->GetName() + ">" : "TWeakObjectPtr";
            else if constexpr (std::is_same_v<T, FLazyObjectProperty>)
                result = prop->PropertyClass ? "TLazyObjectPtr<" + prop->PropertyClass->GetName() + ">" : "TLazyObjectPtr";
            else if constexpr (std::is_same_v<T, FObjectProperty> || std::is_same_v<T, FObjectPropertyBase>)
                result = prop->PropertyClass ? prop->PropertyClass->GetName() : "UObject";
            else if constexpr (std::is_same_v<T, FEnumProperty>)
                result = prop->Enum ? prop->Enum->GetName() : "enum";
            else if constexpr (std::is_same_v<T, FByteProperty>)
                result = prop->Enum ? prop->Enum->GetName() : "byte";
            else if constexpr (std::is_same_v<T, FArrayProperty>)
                result = "TArray<" + (prop->InnerProperty ? GetTypeName(prop->InnerProperty) : "?") + ">";
            else if constexpr (std::is_same_v<T, FSetProperty>)
                result = "TSet<" + (prop->ElementProperty ? GetTypeName(prop->ElementProperty) : "?") + ">";
            else if constexpr (std::is_same_v<T, FMapProperty>)
                result = "TMap<"
                + (prop->KeyProperty ? GetTypeName(prop->KeyProperty) : "?") + ", "
                + (prop->ValueProperty ? GetTypeName(prop->ValueProperty) : "?") + ">";
            else if constexpr (std::is_same_v<T, FBoolProperty>)   result = "bool";
            else if constexpr (std::is_same_v<T, FFloatProperty>)  result = "float";
            else if constexpr (std::is_same_v<T, FDoubleProperty>) result = "double";
            else if constexpr (std::is_same_v<T, FIntProperty>)    result = "int32";
            else if constexpr (std::is_same_v<T, FInt8Property>)   result = "int8";
            else if constexpr (std::is_same_v<T, FInt16Property>)  result = "int16";
            else if constexpr (std::is_same_v<T, FInt64Property>)  result = "int64";
            else if constexpr (std::is_same_v<T, FUInt16Property>) result = "uint16";
            else if constexpr (std::is_same_v<T, FUInt32Property>) result = "uint32";
            else if constexpr (std::is_same_v<T, FUInt64Property>) result = "uint64";
            else if constexpr (std::is_same_v<T, FNameProperty>)  result = "FName";
            else if constexpr (std::is_same_v<T, FStrProperty>)   result = "FString";
            else if constexpr (std::is_same_v<T, FTextProperty>)  result = "FText";
            else if constexpr (std::is_same_v<T, FDelegateProperty>)          result = "FDelegate";
            else if constexpr (std::is_same_v<T, FMulticastDelegateProperty>) result = "FMulticastDelegate";
            else if constexpr (std::is_same_v<T, FInterfaceProperty>)         result = "TScriptInterface";
            else if constexpr (std::is_same_v<T, FFieldPathProperty>)         result = "FFieldPath";
            else result = prop->ClassPrivate ? prop->ClassPrivate->Name.ToString() : "?";
        });
    return result;
}