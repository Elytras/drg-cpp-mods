#pragma once
// Lib_PropertyInspector.h — GObjects search, property read/write, prop command.

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "Lib_Forward.h"
#include "Lib_FField.h"
#include "Lib_ObjectCast.h"
#include "Lib_PropertyAccess.h"
#include "Lib_Print.h"
#include "Lib_Utils.h"
#include "Lib_CommandHandler.h"
#include "SDK/UtfN.hpp"

namespace PropertyInspector
{
    // ---------------------------------------------------------------
    // Search
    // ---------------------------------------------------------------

    inline bool NameMatches(const std::string& haystack, const std::string& needle, bool fuzzy)
    {
        if (needle == "*") return true;
        auto iequal = [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); };
        if (!fuzzy)
            return haystack.size() == needle.size() &&
            std::equal(haystack.begin(), haystack.end(), needle.begin(), iequal);
        return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), iequal) != haystack.end();
    }

    inline int GetMatchScore(const std::string& haystack, const std::string& needle)
    {
        if (needle == "*") return 1;
        if (haystack.size() < needle.size()) return 0;

        auto iequal = [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); };

        // 1. Check Exact
        if (haystack.size() == needle.size()) {
            if (std::equal(haystack.begin(), haystack.end(), needle.begin(), iequal)) return 3;
        }

        // 2. Check Starts With
        if (std::equal(needle.begin(), needle.end(), haystack.begin(), iequal)) return 2;

        // 3. Check Contains
        auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), iequal);
        if (it != haystack.end()) return 1;

        return 0;
    }

    inline UClass* FindClass(const std::string& name, bool fuzzy = false)
    {
        UClass* bestMatch = nullptr;
        int bestScore = 0;

        for (int i = 0; i < UObject::GObjects->Num(); ++i)
        {
            auto* obj = UObject::GObjects->GetByIndex(i);
            if (!obj || !obj->IsA(UClass::StaticClass())) continue;

            int score = GetMatchScore(obj->GetName(), name);

            // If not fuzzy, we only accept score 3 (Exact)
            if (!fuzzy && score < 3) continue;

            if (score > bestScore) {
                bestScore = score;
                bestMatch = static_cast<UClass*>(obj);
                if (bestScore == 3) break; // Optimization: Can't beat an exact match
            }
        }
        return bestMatch;
    }

    inline UObject* FindCDO(const std::string& name, bool fuzzy = false)
    {
        UClass* cls = FindClass(name, fuzzy); return cls ? cls->ClassDefaultObject : nullptr;
    }

    inline UObject* FindInstance(const std::string& name, bool fuzzy = false)
    {
        UObject* bestMatch = nullptr;
        int bestScore = 0;

        for (int i = 0; i < UObject::GObjects->Num(); ++i)
        {
            auto* obj = UObject::GObjects->GetByIndex(i);
            if (!obj || !IsValid(obj) || obj->IsDefaultObject()) continue;

            int score = GetMatchScore(obj->GetName(), name);

            if (!fuzzy && score < 3) continue;

            if (score > bestScore) {
                bestScore = score;
                bestMatch = obj;
                if (bestScore == 3) break;
            }
        }
        return bestMatch;
    }

    inline FProperty* FindProp(UObject* target, const std::string& propName, bool fuzzy = false)
    {
        if (!target || !target->Class) return nullptr;
        for (UClass* cls = target->Class; cls; cls = ObjectCast::Cast<UClass>(cls->SuperStruct))
            for (auto* field : FFieldRange(cls->ChildProperties))
            {
                if (!FieldCast::IsA<FProperty>(field)) continue;
                if (NameMatches(field->Name.ToString(), propName, fuzzy))
                    return static_cast<FProperty*>(field);
            }
        return nullptr;
    }

    inline std::vector<FProperty*> FindAllProps(UObject* target, const std::string& propName, bool fuzzy = false)
    {
        std::vector<FProperty*> results;
        if (!target || !target->Class) return results;
        for (UClass* cls = target->Class; cls; cls = ObjectCast::Cast<UClass>(cls->SuperStruct))
            for (auto* field : FFieldRange(cls->ChildProperties))
            {
                if (!FieldCast::IsA<FProperty>(field)) continue;
                if (NameMatches(field->Name.ToString(), propName, fuzzy))
                    results.push_back(static_cast<FProperty*>(field));
            }
        return results;
    }

    inline std::vector<UObject*> FindAllInstances(const std::string& name, bool fuzzy, UClass* parentClass = nullptr)
    {
        std::vector<UObject*> results;
        static UClass* widgetCls = UObject::FindClass("Class /Script/UMG.UserWidget");
        for (int i = 0; i < UObject::GObjects->Num(); ++i)
        {
            auto* obj = UObject::GObjects->GetByIndex(i);
            if (!obj || obj->IsDefaultObject())                                     continue;
            if (obj->IsA(UClass::StaticClass()))                                    continue;
            if (obj->IsA(UFunction::StaticClass()))                                 continue;
            if (obj->IsA(UPackage::StaticClass()))                                  continue;
            //if (!IsInActiveWorld(obj) && !(widgetCls && obj->IsA(widgetCls)))       continue;
            if (parentClass && !obj->IsA(parentClass))                              continue;
            if (name != "*" && !NameMatches(obj->GetName(), name, fuzzy))           continue;
            results.push_back(obj);
        }
        return results;
    }

    inline std::vector<UObject*> FindAllCDOs(const std::string& name, bool fuzzy)
    {
        std::vector<UObject*> results;
        for (int i = 0; i < UObject::GObjects->Num(); ++i)
        {
            auto* obj = UObject::GObjects->GetByIndex(i);
            if (!obj || !obj->IsA(UClass::StaticClass())) continue;
            auto* cls = static_cast<UClass*>(obj);
            if (!cls->ClassDefaultObject) continue;
            if (NameMatches(obj->GetName(), name, fuzzy)) results.push_back(cls->ClassDefaultObject);
        }
        return results;
    }

    // ---------------------------------------------------------------
    // Target resolution
    // ---------------------------------------------------------------

    enum class TargetKind { CDO, Instance };
    struct ResolvedTarget { UObject* object = nullptr; TargetKind kind = TargetKind::Instance; std::string resolvedName; };

    inline std::optional<ResolvedTarget> Resolve(const std::string& targetName, TargetKind kind, bool fuzzy = false)
    {
        UObject* obj = (kind == TargetKind::CDO) ? FindCDO(targetName, fuzzy) : FindInstance(targetName, fuzzy);
        if (!obj) return std::nullopt;
        return ResolvedTarget{ obj, kind, obj->GetName() };
    }

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
            auto* bp = FieldCast::Cast<FBoolProperty>(prop); if (!bp) return std::nullopt; return ReadBool(target, bp);
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
            auto* bp = FieldCast::Cast<FBoolProperty>(prop); if (!bp) return false; WriteBool(target, bp, value); return true;
        }
        else { *GetPropertyPtr<T>(target, prop->Offset) = value; return true; }
    }

    // ---------------------------------------------------------------
    // WriteProperty
    // ---------------------------------------------------------------

    inline void WriteProperty(UObject* obj, FProperty* prop, uintptr_t writeBase,
        FProperty* writeProp, const std::string& valueStr,
        const std::string& baseName, bool hadIndex, int32 elementIndex)
    {
        auto* fakeBase = reinterpret_cast<UObject*>(writeBase);
        FieldCast::Visit(writeProp, [&](auto* p)
            {
                using T = std::remove_pointer_t<decltype(p)>;
                try
                {
                    if constexpr (std::is_same_v<T, FBoolProperty>)   WriteBool(fakeBase, p, valueStr == "1" || valueStr == "true");
                    else if constexpr (std::is_same_v<T, FIntProperty>)    *GetPropertyPtr<int32>(writeBase, p->Offset) = std::stoi(valueStr);
                    else if constexpr (std::is_same_v<T, FFloatProperty>)  *GetPropertyPtr<float>(writeBase, p->Offset) = std::stof(valueStr);
                    else if constexpr (std::is_same_v<T, FDoubleProperty>) *GetPropertyPtr<double>(writeBase, p->Offset) = std::stod(valueStr);
                    else if constexpr (std::is_same_v<T, FInt64Property>)  *GetPropertyPtr<int64>(writeBase, p->Offset) = std::stoll(valueStr);
                    else if constexpr (std::is_same_v<T, FByteProperty>)   *GetPropertyPtr<uint8>(writeBase, p->Offset) = static_cast<uint8>(std::stoi(valueStr));
                    else if constexpr (std::is_same_v<T, FStrProperty>)
                    {
                        std::wstring w(valueStr.begin(), valueStr.end()); *GetPropertyPtr<UC::FString>(writeBase, p->Offset) = UC::FString(w.c_str());
                    }
                    else spdlog::warn("[prop] set not supported for this property type");
                    spdlog::info("[prop] Set '{}.{}{}' = {}", obj->GetName(), baseName,
                        hadIndex ? "[" + std::to_string(elementIndex) + "]" : "", valueStr);
                }
                catch (...) { spdlog::warn("[prop] Failed to parse value '{}'", valueStr); }
            });
    }

    // ---------------------------------------------------------------
    // ComputeParmsSize / WriteParam
    // ---------------------------------------------------------------

    inline int32 ComputeParmsSize(UFunction* Func)
    {
        int32 size = 0;
        for (auto* field : FFieldRange(Func->ChildProperties))
        {
            if (!FieldCast::IsA<FProperty>(field)) continue;
            auto* Prop = static_cast<FProperty*>(field);
            auto  pf = static_cast<EPropertyFlags>(Prop->PropertyFlags);
            if (!(pf & EPropertyFlags::Parm))    continue;
            if (pf & EPropertyFlags::ReturnParm) continue;
            int32 end = Prop->Offset + Prop->ElementSize;
            if (end > size) size = end;
        }
        return (size + 7) & ~7;
    }

    inline bool WriteParam(FProperty* Prop, const std::string& token, uint8_t* parms)
    {
        bool handled = false;
        FieldCast::Visit(Prop, [&]<typename T>(T * p)
        {
            auto base = reinterpret_cast<uintptr_t>(parms);
            if constexpr (std::is_same_v<T, FBoolProperty>)
            {
                bool v = (token == "1" || token == "true");
                if (p->FieldMask == 0xFF) *GetPropertyPtr<bool>(base, p->Offset) = v;
                else { uint8_t* byte = GetPropertyPtr<uint8_t>(base, p->Offset + p->ByteOffset); if (v) *byte |= p->FieldMask; else *byte &= ~p->FieldMask; }
                handled = true;
            }
            else if constexpr (std::is_same_v<T, FInt8Property>) { *GetPropertyPtr<int8_t>(base, p->Offset) = static_cast<int8_t>  (SafeStoll(token));  handled = true; }
            else if constexpr (std::is_same_v<T, FInt16Property>) { *GetPropertyPtr<int16_t>(base, p->Offset) = static_cast<int16_t> (SafeStoll(token));  handled = true; }
            else if constexpr (std::is_same_v<T, FIntProperty>) { *GetPropertyPtr<int32_t>(base, p->Offset) = static_cast<int32_t> (SafeStoll(token));  handled = true; }
            else if constexpr (std::is_same_v<T, FInt64Property>) { *GetPropertyPtr<int64_t>(base, p->Offset) = SafeStoll(token);                         handled = true; }
            else if constexpr (std::is_same_v<T, FByteProperty>) { *GetPropertyPtr<uint8_t>(base, p->Offset) = static_cast<uint8_t> (SafeStoull(token)); handled = true; }
            else if constexpr (std::is_same_v<T, FUInt16Property>) { *GetPropertyPtr<uint16_t>(base, p->Offset) = static_cast<uint16_t>(SafeStoull(token)); handled = true; }
            else if constexpr (std::is_same_v<T, FUInt32Property>) { *GetPropertyPtr<uint32_t>(base, p->Offset) = static_cast<uint32_t>(SafeStoull(token)); handled = true; }
            else if constexpr (std::is_same_v<T, FUInt64Property>) { *GetPropertyPtr<uint64_t>(base, p->Offset) = SafeStoull(token);                        handled = true; }
            else if constexpr (std::is_same_v<T, FFloatProperty>) { *GetPropertyPtr<float>(base, p->Offset) = SafeStof(token);                          handled = true; }
            else if constexpr (std::is_same_v<T, FDoubleProperty>) { *GetPropertyPtr<double>(base, p->Offset) = SafeStod(token);                          handled = true; }
            else if constexpr (std::is_same_v<T, FEnumProperty>)
            {
                int64_t val = SafeStoll(token);
                if (p->UnderlayingProperty)
                {
                    uint8_t* slot = GetPropertyPtr<uint8_t>(base, p->Offset);
                    switch (p->UnderlayingProperty->ElementSize)
                    {
                    case 1: *reinterpret_cast<uint8_t*> (slot) = static_cast<uint8_t> (val); break;
                    case 2: *reinterpret_cast<uint16_t*>(slot) = static_cast<uint16_t>(val); break;
                    case 4: *reinterpret_cast<uint32_t*>(slot) = static_cast<uint32_t>(val); break;
                    case 8: *reinterpret_cast<uint64_t*>(slot) = static_cast<uint64_t>(val); break;
                    default: spdlog::warn("[cmd:call] unexpected enum backing size {}", p->UnderlayingProperty->ElementSize);
                    }
                }
                else *GetPropertyPtr<uint8_t>(base, p->Offset) = static_cast<uint8_t>(val);
                handled = true;
            }
            else if constexpr (std::is_same_v<T, FStrProperty>)
            {
                new (GetPropertyPtr<FString>(base, p->Offset)) FString(ToFString(token)); handled = true;
            }
            else if constexpr (std::is_same_v<T, FNameProperty>)
            {
                new (GetPropertyPtr<FName>(base, p->Offset)) FName(BFIU::StringToName(UtfN::StringToWString(token).c_str())); handled = true;
            }
            else if constexpr (std::is_same_v<T, FStructProperty>)
            {
                std::string sname = p->Struct->GetName();
                if (sname == "Vector" || sname == "Vector_NetQuantize" || sname == "Vector_NetQuantize10" || sname == "Vector_NetQuantize100" || sname == "Vector_NetQuantizeNormal")
                {
                    float x = 0, y = 0, z = 0; sscanf_s(token.c_str(), "%f,%f,%f", &x, &y, &z); *GetPropertyPtr<FVector>(base, p->Offset) = { x,y,z };
                }
                else if (sname == "Rotator")
                {
                    float p2 = 0, y = 0, r = 0; sscanf_s(token.c_str(), "%f,%f,%f", &p2, &y, &r); *GetPropertyPtr<FRotator>(base, p->Offset) = { p2,y,r };
                }
                else if (sname == "Transform")
                {
                    float rx = 0, ry = 0, rz = 0, rw = 1, tx = 0, ty = 0, tz = 0, sx = 1, sy = 1, sz = 1;
                    sscanf_s(token.c_str(), "%f,%f,%f,%f %f,%f,%f %f,%f,%f", &rx, &ry, &rz, &rw, &tx, &ty, &tz, &sx, &sy, &sz);
                    auto* t = GetPropertyPtr<FTransform>(base, p->Offset);
                    t->Rotation = { rx,ry,rz,rw }; t->Translation = { tx,ty,tz }; t->Scale3D = { sx,sy,sz };
                }
                else spdlog::warn("[cmd:call] struct '{}' not handled for '{}', leaving zeroed.", sname, p->Name.ToString());
                handled = true;
            }
            else if constexpr (std::is_same_v<T, FObjectProperty> || std::is_same_v<T, FObjectPropertyBase> || std::is_same_v<T, FClassProperty> || std::is_same_v<T, FWeakObjectProperty>)
            {
                UObject* found = nullptr;
                if (token != "null" && token != "none" && token != "0")
                {
                    for (int32 i = 0; i < UObject::GObjects->NumElements; ++i)
                    {
                        auto* obj = UObject::GObjects->GetByIndex(i); if (obj && obj->GetName() == token) { found = obj; break; }
                    }
                    if (!found) spdlog::warn("[cmd:call] object '{}' not found in GObjects.", token);
                }
                *GetPropertyPtr<UObject*>(base, p->Offset) = found;
                handled = true;
            }
            else spdlog::warn("[cmd:call] unhandled property type for '{}', leaving zeroed.", p->Name.ToString());
        });
        return handled;
    }

    // ---------------------------------------------------------------
    // Map helpers
    // ---------------------------------------------------------------

    struct MapLayout { uintptr_t dataPtr; int32 numAllocated; const uint32* bitData; int32 pairStride; };

    inline bool GetMapLayout(UObject* obj, FMapProperty* mapProp, MapLayout& out)
    {
        const uintptr_t mb = reinterpret_cast<uintptr_t>(obj) + mapProp->Offset;
        out.dataPtr = *reinterpret_cast<const uintptr_t*>(mb);
        out.numAllocated = *reinterpret_cast<const int32*>(mb + 0x08);
        const uintptr_t bab = mb + 0x10;
        const int32* id = reinterpret_cast<const int32*>(bab);
        const int32* sd = *reinterpret_cast<int32* const*>(bab + 0x10);
        out.bitData = sd ? reinterpret_cast<const uint32*>(sd) : reinterpret_cast<const uint32*>(id);
        out.pairStride = (((mapProp->ValueProperty->Offset + mapProp->ValueProperty->ElementSize) + 7) & ~7) + 8;
        return out.dataPtr != 0 && out.numAllocated > 0;
    }

    inline bool KeyMatches(uintptr_t pairBase, FProperty* keyProp, const std::string& keyStr)
    {
        return FieldCast::Visit(keyProp, [&](auto* p) -> bool
            {
                using T = std::remove_pointer_t<decltype(p)>;
                if constexpr (std::is_same_v<T, FStrProperty>)
                {
                    auto* fstr = GetPropertyPtr<UC::FString>(pairBase, p->Offset); if (!fstr || !fstr->IsValid()) return keyStr.empty(); std::wstring w(keyStr.begin(), keyStr.end()); return wcsncmp(fstr->GetDataPtr(), w.c_str(), (std::max)(fstr->Num(), (int32)w.size())) == 0;
                }
                else if constexpr (std::is_same_v<T, FNameProperty>)
                {
                    auto* fn = GetPropertyPtr<SDK::FName>(pairBase, p->Offset); if (!fn) return false; std::string ns = fn->ToString(); auto ie = [](unsigned char a, unsigned char b) {return std::tolower(a) == std::tolower(b);}; return ns.size() == keyStr.size() && std::equal(ns.begin(), ns.end(), keyStr.begin(), ie);
                }
                else if constexpr (std::is_same_v<T, FIntProperty>) { try { return *GetPropertyPtr<int32>(pairBase, p->Offset) == std::stoi(keyStr); } catch (...) { return false; } }
                else if constexpr (std::is_same_v<T, FInt64Property>) { try { return *GetPropertyPtr<int64>(pairBase, p->Offset) == std::stoll(keyStr); } catch (...) { return false; } }
                else if constexpr (std::is_same_v<T, FFloatProperty>) { try { return std::abs(*GetPropertyPtr<float>(pairBase, p->Offset) - std::stof(keyStr)) < 1e-4f; } catch (...) { return false; } }
                else if constexpr (std::is_same_v<T, FDoubleProperty>) { try { return std::abs(*GetPropertyPtr<double>(pairBase, p->Offset) - std::stod(keyStr)) < 1e-7; } catch (...) { return false; } }
                else if constexpr (std::is_same_v<T, FBoolProperty>) { return ReadBool(reinterpret_cast<UObject*>(pairBase), p) == (keyStr == "1" || keyStr == "true"); }
                else { spdlog::warn("[prop] Key type not supported for key matching"); return false; }
            });
    }

    inline void ExecuteMapGet(UObject* obj, FMapProperty* mapProp, const std::string& keyStr)
    {
        MapLayout l; if (!GetMapLayout(obj, mapProp, l)) { spdlog::warn("[prop] Map '{}' is empty", mapProp->Name.ToString());return; }
        for (int32 i = 0;i < l.numAllocated;++i)
        {
            if (!l.bitData || (!(l.bitData[i / 32] & (1u << (i % 32))))) continue; uintptr_t pb = l.dataPtr + i * l.pairStride; if (!KeyMatches(pb, mapProp->KeyProperty, keyStr)) continue; std::vector<bool> a{}; PrintFieldValue(pb, mapProp->ValueProperty, a, true, "val"); return;
        }
        spdlog::warn("[prop] Key '{}' not found in map '{}'", keyStr, mapProp->Name.ToString());
    }

    inline void ExecuteMapGetByIndex(UObject* obj, FMapProperty* mapProp, int32 index)
    {
        MapLayout l; if (!GetMapLayout(obj, mapProp, l)) { spdlog::warn("[prop] Map empty");return; }
        int32 live = 0;
        for (int32 i = 0;i < l.numAllocated;++i)
        {
            if (!l.bitData || (!(l.bitData[i / 32] & (1u << (i % 32))))) continue; if (live != index) { ++live;continue; } uintptr_t pb = l.dataPtr + i * l.pairStride; std::vector<bool> a{}; PrintFieldValue(pb, mapProp->KeyProperty, a, false, "key"); PrintFieldValue(pb, mapProp->ValueProperty, a, true, "val"); return;
        }
        spdlog::warn("[prop] Map index {} out of range", index);
    }

    inline void ExecuteMapSet(UObject* obj, FMapProperty* mapProp, const std::string& keyStr, const std::string& valueStr)
    {
        MapLayout l; if (!GetMapLayout(obj, mapProp, l)) { spdlog::warn("[prop] Map empty");return; }
        for (int32 i = 0;i < l.numAllocated;++i)
        {
            if (!l.bitData || (!(l.bitData[i / 32] & (1u << (i % 32))))) continue;
            uintptr_t pb = l.dataPtr + i * l.pairStride; if (!KeyMatches(pb, mapProp->KeyProperty, keyStr)) continue;
            auto* fb = reinterpret_cast<UObject*>(pb);
            FieldCast::Visit(mapProp->ValueProperty, [&](auto* p) { using T = std::remove_pointer_t<decltype(p)>;
            try {
                if constexpr (std::is_same_v<T, FBoolProperty>)   WriteBool(fb, p, valueStr == "1" || valueStr == "true");
                else if constexpr (std::is_same_v<T, FIntProperty>)   *GetPropertyPtr<int32>(pb, p->Offset) = std::stoi(valueStr);
                else if constexpr (std::is_same_v<T, FFloatProperty>)  *GetPropertyPtr<float>(pb, p->Offset) = std::stof(valueStr);
                else if constexpr (std::is_same_v<T, FDoubleProperty>) *GetPropertyPtr<double>(pb, p->Offset) = std::stod(valueStr);
                else if constexpr (std::is_same_v<T, FInt64Property>)  *GetPropertyPtr<int64>(pb, p->Offset) = std::stoll(valueStr);
                else if constexpr (std::is_same_v<T, FByteProperty>)   *GetPropertyPtr<uint8>(pb, p->Offset) = static_cast<uint8>(std::stoi(valueStr));
                else if constexpr (std::is_same_v<T, FStrProperty>) { std::wstring w(valueStr.begin(), valueStr.end());*GetPropertyPtr<UC::FString>(pb, p->Offset) = UC::FString(w.c_str()); }
                else spdlog::warn("[prop] set not supported for value type '{}'", p->ClassPrivate->Name.ToString());
                spdlog::info("[prop] Set map '{}'{{{}}} = {}", mapProp->Name.ToString(), keyStr, valueStr);
            }
            catch (...) { spdlog::warn("[prop] Failed to parse value '{}'", valueStr); }
                });
            return;
        }
        spdlog::warn("[prop] Key '{}' not found", keyStr);
    }

    inline void ExecuteMapSetByIndex(UObject* obj, FMapProperty* mapProp, int32 index, const std::string& valueStr)
    {
        MapLayout l; if (!GetMapLayout(obj, mapProp, l)) { spdlog::warn("[prop] Map empty");return; }
        int32 live = 0;
        for (int32 i = 0;i < l.numAllocated;++i)
        {
            if (!l.bitData || (!(l.bitData[i / 32] & (1u << (i % 32))))) continue; if (live != index) { ++live;continue; } uintptr_t pb = l.dataPtr + i * l.pairStride; WriteProperty(obj, mapProp, pb, mapProp->ValueProperty, valueStr, mapProp->Name.ToString(), true, index); return;
        }
        spdlog::warn("[prop] Map index {} out of range", index);
    }

    // ---------------------------------------------------------------
    // ExecuteOnTarget
    // ---------------------------------------------------------------

    inline void Dump(UObject* target, UClass* outerBase = nullptr) { DumpItemProperties(target, outerBase); }

    inline void ExecuteOnTarget(UObject* obj, const std::string& action,
        const std::string& propName, const std::string& valueStr)
    {
        if (action == "dump") { Dump(obj); return; }

        std::string baseName = propName;
        bool hadIndex = false; int32 elementIndex = -1;
        bool hadKey = false; std::string mapKey;

        if (const auto lb = propName.rfind('['); lb != std::string::npos)
        {
            const auto rb = propName.rfind(']');
            if (rb != std::string::npos && rb > lb)
            {
                try { int32 p = std::stoi(propName.substr(lb + 1, rb - lb - 1)); if (p < 0) { spdlog::warn("[prop] Negative index");return; } elementIndex = p; hadIndex = true; baseName = propName.substr(0, lb); }
                catch (...) { spdlog::warn("[prop] Invalid index in '{}'", propName); return; }
            }
        }
        else if (const auto lb2 = propName.rfind('{'); lb2 != std::string::npos)
        {
            const auto rb2 = propName.rfind('}');
            if (rb2 != std::string::npos && rb2 > lb2) { mapKey = propName.substr(lb2 + 1, rb2 - lb2 - 1); hadKey = true; baseName = propName.substr(0, lb2); }
            else { spdlog::warn("[prop] Unmatched '{{' in '{}'", propName); return; }
        }

        if (action == "list")
        {
            auto props = FindAllProps(obj, baseName, true);
            spdlog::info("[prop] {} properties matching '{}':", props.size(), baseName);
            std::vector<bool> a{};
            for (size_t i = 0;i < props.size();++i) PrintFieldValue(reinterpret_cast<uintptr_t>(obj), props[i], a, i == props.size() - 1);
            return;
        }

        if (action == "get")
        {
            auto* prop = FindProp(obj, baseName, false);
            if (!prop) { spdlog::warn("[prop] Property '{}' not found", baseName); return; }
            if (hadKey) { auto* mp = FieldCast::Cast<FMapProperty>(prop); if (!mp) { spdlog::warn("[prop] '{}' not TMap", baseName);return; } ExecuteMapGet(obj, mp, mapKey); return; }
            if (hadIndex)
            {
                auto* ap = FieldCast::Cast<FArrayProperty>(prop);
                if (!ap) { auto* mp = FieldCast::Cast<FMapProperty>(prop); if (!mp) { spdlog::warn("[prop] '{}' not TArray/TMap", baseName);return; } ExecuteMapGetByIndex(obj, mp, elementIndex); return; }
                const auto& arr = *GetPropertyPtr<UC::TArray<uint8>>(obj, ap->Offset);
                if (!arr.IsValid() || elementIndex >= arr.Num()) { spdlog::warn("[prop] Index {} out of range", elementIndex);return; }
                uintptr_t eb = reinterpret_cast<uintptr_t>(arr.GetDataPtr()) + elementIndex * ap->InnerProperty->ElementSize;
                std::vector<bool> a{}; PrintFieldValue(eb, ap->InnerProperty, a, true); return;
            }
            PrintFieldValue(obj, prop, {}, true); return;
        }

        if (action == "set")
        {
            auto* prop = FindProp(obj, baseName, false);
            if (!prop) { spdlog::warn("[prop] Property '{}' not found", baseName); return; }
            if (hadKey) { auto* mp = FieldCast::Cast<FMapProperty>(prop); if (!mp) { spdlog::warn("[prop] '{}' not TMap", baseName);return; } ExecuteMapSet(obj, mp, mapKey, valueStr); return; }
            uintptr_t wb = reinterpret_cast<uintptr_t>(obj); FProperty* wp = prop;
            if (hadIndex)
            {
                auto* ap = FieldCast::Cast<FArrayProperty>(prop);
                if (!ap) { auto* mp = FieldCast::Cast<FMapProperty>(prop); if (!mp) { spdlog::warn("[prop] '{}' not TArray/TMap", baseName);return; } ExecuteMapSetByIndex(obj, mp, elementIndex, valueStr); return; }
                const auto& arr = *GetPropertyPtr<UC::TArray<uint8>>(obj, ap->Offset);
                if (!arr.IsValid() || elementIndex >= arr.Num()) { spdlog::warn("[prop] Index {} out of range", elementIndex);return; }
                wb = reinterpret_cast<uintptr_t>(arr.GetDataPtr()) + elementIndex * ap->InnerProperty->ElementSize;
                wp = ap->InnerProperty;
            }
            WriteProperty(obj, prop, wb, wp, valueStr, baseName, hadIndex, elementIndex);
        }
    }

    // ---------------------------------------------------------------
    // Pending selection + DispatchCommand
    // ---------------------------------------------------------------

    struct PendingSelection { std::vector<UObject*> candidates; std::string pendingAction, pendingProp, pendingValue; };
    inline std::optional<PendingSelection> g_Pending;

    inline void DispatchCommand(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 2) { spdlog::info("[prop] usage: prop <cdo|obj> <n> <dump|get|set|list> [prop] [value] [fuzzy]"); return; }

        if (ctx.Arg(1) == "pick")
        {
            if (!g_Pending) { spdlog::warn("[prop] No pending selection."); return; }
            int idx = -1; try { idx = std::stoi(ctx.Arg(2)); }
            catch (...) {}
            if (idx < 0 || idx >= (int)g_Pending->candidates.size()) { spdlog::warn("[prop] Index {} out of range", idx);return; }
            UObject* chosen = g_Pending->candidates[idx];
            if (!IsValidRaw(chosen)) { spdlog::warn("[prop] Candidate no longer valid.");g_Pending.reset();return; }
            spdlog::info("[prop] Selected: '{}'", chosen->GetName());
            ExecuteOnTarget(chosen, g_Pending->pendingAction, g_Pending->pendingProp, g_Pending->pendingValue);
            g_Pending.reset(); return;
        }

        if (ctx.ArgCount() < 4) { spdlog::info("[prop] usage: prop <cdo|obj> <n> <dump|get|set|list> [prop] [value] [fuzzy]"); return; }

        const std::string& kindStr = ctx.Arg(1), & name = ctx.Arg(2), & action = ctx.Arg(3);
        const bool hasPropSlot = (action != "dump");
        const std::string propName = hasPropSlot ? ctx.Arg(4) : "", value = hasPropSlot ? ctx.Arg(5) : "";
        const size_t flagStart = hasPropSlot ? 6 : 4;

        bool fuzzy = false; std::string parentClassName;
        for (size_t i = flagStart;i < ctx.ArgCount();++i)
        {
            if (ctx.Arg(i) == "fuzzy") fuzzy = true; if (ctx.Arg(i) == "class" && i + 1 < ctx.ArgCount()) parentClassName = ctx.Arg(++i);
        }

        UClass* parentClass = nullptr;
        if (!parentClassName.empty())
        {
            parentClass = PropertyInspector::FindClass(parentClassName, false);
            if (!parentClass) { parentClass = PropertyInspector::FindClass(parentClassName, true); if (parentClass) spdlog::info("[prop] Class '{}' fuzzy→'{}'", parentClassName, parentClass->GetName()); }
            if (!parentClass) spdlog::warn("[prop] Parent class '{}' not found", parentClassName);
        }

        bool isCDO = (kindStr == "cdo");
        auto candidates = isCDO ? FindAllCDOs(name, fuzzy) : FindAllInstances(name, fuzzy, parentClass);

        if (!propName.empty() && action != "dump")
        {
            std::string bp = propName;
            if (const auto lb = propName.rfind('[');lb != std::string::npos) bp = propName.substr(0, lb);
            else if (const auto lb2 = propName.rfind('{');lb2 != std::string::npos) bp = propName.substr(0, lb2);
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](UObject* o) {return FindProp(o, bp, false) == nullptr;}), candidates.end());
        }

        if (candidates.empty())
        {
            spdlog::warn("[prop] No {} matching '{}'{}{}", isCDO ? "CDOs" : "instances", name, fuzzy ? "" : " (try 'fuzzy')", propName.empty() ? "" : " with property '" + propName + "'"); return;
        }

        if (candidates.size() == 1)
        {
            spdlog::info("[prop] Resolved: '{}'", candidates[0]->GetName()); ExecuteOnTarget(candidates[0], action, propName, value); return;
        }

        spdlog::info("[prop] {} candidates for '{}', use 'prop pick <index>':", candidates.size(), name);
        std::unordered_map<std::string, int> cnc;
        for (auto* o : candidates) if (o->Class) cnc[o->Class->GetName()]++;
        for (size_t i = 0;i < candidates.size();++i)
        {
            UClass* cls = candidates[i]->Class;
            bool ambiguous = cls && cnc[cls->GetName()] > 1;
            std::string label = ambiguous ? Kismet::Conv_SoftClassReferenceToString(Kismet::Conv_ClassToSoftClassReference(cls)).ToString() : (cls ? cls->GetName() : "?");
            spdlog::info("  [{}] {} ({})", i, candidates[i]->GetName(), label);
        }
        g_Pending = PendingSelection{ candidates,action,propName,value };
    }

} // namespace PropertyInspector