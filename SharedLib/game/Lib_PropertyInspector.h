#pragma once
// Lib_PropertyInspector.h — GObjects search, property read/write, prop command.

#include <optional>
#include <string>
#include <vector>
#include "Lib_Forward.h"
#include "Lib_ObjectCast.h"
#include "Lib_PropertyAccess.h"
#include "Lib_Utils.h"
#include "Lib_CommandHandler.h"

namespace PropertyInspector
{
    using namespace SDK;

    // ---------------------------------------------------------------
    // Structs / enums
    // ---------------------------------------------------------------

    enum class TargetKind { CDO, Instance };
    struct ResolvedTarget { UObject* object = nullptr; TargetKind kind = TargetKind::Instance; std::string resolvedName; };
    struct MapLayout      { uintptr_t dataPtr; int32 numAllocated; const uint32* bitData; int32 pairStride; };
    // ---------------------------------------------------------------
    // Search
    // ---------------------------------------------------------------

    bool                  NameMatches      (const std::string& haystack, const std::string& needle, bool fuzzy);
    int                   GetMatchScore    (const std::string& haystack, const std::string& needle);
    UClass*               FindClass        (const std::string& name, bool fuzzy = false);
    UObject*              FindCDO          (const std::string& name, bool fuzzy = false);
    UObject*              FindInstance     (const std::string& name, bool fuzzy = false);
    FProperty*            FindProp         (UObject* target, const std::string& propName, bool fuzzy = false);
    std::vector<FProperty*> FindAllProps   (UObject* target, const std::string& propName, bool fuzzy = false);
    std::vector<UObject*> FindAllInstances (const std::string& name, bool fuzzy, UClass* parentClass = nullptr);
    std::vector<UObject*> FindAllCDOs      (const std::string& name, bool fuzzy);

    // ---------------------------------------------------------------
    // Target resolution
    // ---------------------------------------------------------------

    std::optional<ResolvedTarget> Resolve(const std::string& targetName, TargetKind kind, bool fuzzy = false);

    // ---------------------------------------------------------------
    // Typed get/set
    // ---------------------------------------------------------------

    template<typename T>
    std::optional<T> Get(UObject* target, const std::string& propName, bool fuzzy = false)
    {
        auto* prop = FindProp(target, propName, fuzzy);
        if (!prop) return std::nullopt;
        if constexpr (std::is_same_v<T, bool>)
        {
            auto* bp = FieldCast::Cast<FBoolProperty>(prop);
            if (!bp) return std::nullopt;
            return ReadBool(target, bp);
        }
        else return *GetPropertyPtr<T>(target, prop->Offset);
    }

    template<typename T>
    bool Set(UObject* target, const std::string& propName, T value, bool fuzzy = false)
    {
        auto* prop = FindProp(target, propName, fuzzy);
        if (!prop) return false;
        if constexpr (std::is_same_v<T, bool>)
        {
            auto* bp = FieldCast::Cast<FBoolProperty>(prop);
            if (!bp) return false;
            WriteBool(target, bp, value);
            return true;
        }
        else { *GetPropertyPtr<T>(target, prop->Offset) = value; return true; }
    }

    // ---------------------------------------------------------------
    // WriteProperty / ComputeParmsSize / WriteParam
    // ---------------------------------------------------------------

    void  WriteProperty     (UObject* obj, FProperty* prop, uintptr_t writeBase,
                             FProperty* writeProp, const std::string& valueStr,
                             const std::string& baseName, bool hadIndex, int32 elementIndex);
    int32 ComputeParmsSize  (UFunction* Func);
    bool  WriteParam        (FProperty* Prop, const std::string& token, uint8_t* parms);

    // ---------------------------------------------------------------
    // Map helpers
    // ---------------------------------------------------------------

    bool GetMapLayout       (UObject* obj, FMapProperty* mapProp, MapLayout& out);
    bool KeyMatches         (uintptr_t pairBase, FProperty* keyProp, const std::string& keyStr);
    void ExecuteMapGet      (UObject* obj, FMapProperty* mapProp, const std::string& keyStr);
    void ExecuteMapGetByIndex(UObject* obj, FMapProperty* mapProp, int32 index);
    void ExecuteMapSet      (UObject* obj, FMapProperty* mapProp, const std::string& keyStr, const std::string& valueStr);
    void ExecuteMapSetByIndex(UObject* obj, FMapProperty* mapProp, int32 index, const std::string& valueStr);

    // ---------------------------------------------------------------
    // ExecuteOnTarget / DispatchCommand
    // ---------------------------------------------------------------

    void Dump           (UObject* target, UClass* outerBase = nullptr);
    void ExecuteOnTarget(UObject* obj, const std::string& action,
                         const std::string& propName, const std::string& valueStr);
    void DispatchCommand(const CommandContext& ctx);

} // namespace PropertyInspector
