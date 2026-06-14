#include "Lib_PropertyInspector.h"
#include "Lib_Print.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace PropertyInspector
{

namespace {
    struct PendingSelection { std::vector<UObject*> candidates; std::string pendingAction, pendingProp, pendingValue; };
    std::optional<PendingSelection> g_Pending;

    // Returns the component's owning actor via Outer (the UObject outer chain is
    // always set for component instances).  Returns nullptr for non-components or
    // when Outer is absent.  The static class lookup is cached on first call.
    AActor* GetComponentOwner(const UObject* obj)
    {
        const UActorComponent* comp = ObjectCast::Cast<UActorComponent>(obj);
        if (!comp) return nullptr;
        return comp->GetOwner();
    }
}

// ---------------------------------------------------------------
// Search
// ---------------------------------------------------------------

bool NameMatches(const std::string& haystack, const std::string& needle, bool fuzzy)
{
    if (needle == "*") return true;
    auto iequal = [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); };
    if (!fuzzy)
        return haystack.size() == needle.size() &&
               std::equal(haystack.begin(), haystack.end(), needle.begin(), iequal);
    return std::search(haystack.begin(), haystack.end(),
                       needle.begin(), needle.end(), iequal) != haystack.end();
}

int GetMatchScore(const std::string& haystack, const std::string& needle)
{
    if (needle == "*") return 1;
    if (haystack.size() < needle.size()) return 0;
    auto iequal = [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); };
    if (haystack.size() == needle.size())
        if (std::equal(haystack.begin(), haystack.end(), needle.begin(), iequal)) return 3;
    if (std::equal(needle.begin(), needle.end(), haystack.begin(), iequal)) return 2;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), iequal);
    if (it != haystack.end()) return 1;
    return 0;
}

// UE strips the leading A/U/F/E/I/S/T prefix when registering reflection names,
// so `APlayerCharacter` in C++ becomes `PlayerCharacter` in GObjects. Try the
// stripped form as a fallback so users can write either spelling.
static std::string StripUEPrefix(const std::string& s)
{
    if (s.size() < 2) return s;
    const char p = s[0];
    if ((p == 'A' || p == 'U' || p == 'F' || p == 'E' ||
         p == 'I' || p == 'S' || p == 'T') &&
        std::isupper(static_cast<unsigned char>(s[1])))
        return s.substr(1);
    return s;
}

UClass* FindClass(const std::string& name, bool fuzzy)
{
    const std::string stripped = StripUEPrefix(name);
    const bool hasStripped = stripped != name;

    UClass* bestMatch = nullptr;
    int bestScore = 0;
    for (int i = 0; i < UObject::GObjects->Num(); ++i)
    {
        auto* obj = UObject::GObjects->GetByIndex(i);
        if (!obj || !obj->IsA(UClass::StaticClass())) continue;
        const std::string objName = obj->GetName();
        int score = GetMatchScore(objName, name);
        if (hasStripped)
        {
            int sscore = GetMatchScore(objName, stripped);
            if (sscore > score) score = sscore;
        }
        if (!fuzzy && score < 3) continue;
        if (score > bestScore) {
            bestScore = score;
            bestMatch = static_cast<UClass*>(obj);
            if (bestScore == 3) break;
        }
    }
    return bestMatch;
}

UObject* FindCDO(const std::string& name, bool fuzzy)
{
    UClass* cls = FindClass(name, fuzzy);
    return cls ? cls->ClassDefaultObject : nullptr;
}

UObject* FindInstance(const std::string& name, bool fuzzy)
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

FProperty* FindProp(UObject* target, const std::string& propName, bool fuzzy)
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

std::vector<FProperty*> FindAllProps(UObject* target, const std::string& propName, bool fuzzy)
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

std::vector<UObject*> FindAllInstances(const std::string& name, bool fuzzy, UClass* parentClass)
{
    std::vector<UObject*> results;
    for (int i = 0; i < UObject::GObjects->Num(); ++i)
    {
        auto* obj = UObject::GObjects->GetByIndex(i);
        if (!obj || obj->IsDefaultObject())                           continue;
        if (obj->IsA(UClass::StaticClass()))                          continue;
        if (obj->IsA(UFunction::StaticClass()))                       continue;
        if (obj->IsA(UPackage::StaticClass()))                        continue;
        if (!IsValidRaw(obj))                                         continue;
        if (parentClass && !obj->IsA(parentClass))                    continue;
        if (name != "*" && !NameMatches(obj->GetName(), name, fuzzy)) continue;
        results.push_back(obj);
    }
    return results;
}

std::vector<UObject*> FindAllCDOs(const std::string& name, bool fuzzy)
{
    std::vector<UObject*> results;
    for (int i = 0; i < UObject::GObjects->Num(); ++i)
    {
        auto* obj = UObject::GObjects->GetByIndex(i);
        if (!obj || !obj->IsA(UClass::StaticClass())) continue;
        auto* cls = static_cast<UClass*>(obj);
        if (!cls->ClassDefaultObject) continue;
        if (NameMatches(obj->GetName(), name, fuzzy))
            results.push_back(cls->ClassDefaultObject);
    }
    return results;
}

// ---------------------------------------------------------------
// Target resolution
// ---------------------------------------------------------------

std::optional<ResolvedTarget> Resolve(const std::string& targetName, TargetKind kind, bool fuzzy)
{
    UObject* obj = (kind == TargetKind::CDO) ? FindCDO(targetName, fuzzy) : FindInstance(targetName, fuzzy);
    if (!obj) return std::nullopt;
    return ResolvedTarget{ obj, kind, obj->GetName() };
}

// ---------------------------------------------------------------
// WriteProperty
// ---------------------------------------------------------------

void WriteProperty(UObject* obj, FProperty* prop, uintptr_t writeBase,
    FProperty* writeProp, const std::string& valueStr,
    const std::string& baseName, bool hadIndex, int32 elementIndex)
{
    auto* fakeBase = reinterpret_cast<UObject*>(writeBase);
    FieldCast::Visit(writeProp, [&](auto* p)
        {
            using T = std::remove_pointer_t<decltype(p)>;
            try
            {
                if constexpr (std::is_same_v<T, FBoolProperty>)
                    WriteBool(fakeBase, p, valueStr == "1" || valueStr == "true");
                else if constexpr (std::is_same_v<T, FIntProperty>)
                    *GetPropertyPtr<int32>(writeBase, p->Offset) = std::stoi(valueStr);
                else if constexpr (std::is_same_v<T, FFloatProperty>)
                    *GetPropertyPtr<float>(writeBase, p->Offset) = std::stof(valueStr);
                else if constexpr (std::is_same_v<T, FDoubleProperty>)
                    *GetPropertyPtr<double>(writeBase, p->Offset) = std::stod(valueStr);
                else if constexpr (std::is_same_v<T, FInt64Property>)
                    *GetPropertyPtr<int64>(writeBase, p->Offset) = std::stoll(valueStr);
                else if constexpr (std::is_same_v<T, FByteProperty>)
                    *GetPropertyPtr<uint8>(writeBase, p->Offset) = static_cast<uint8>(std::stoi(valueStr));
                else if constexpr (std::is_same_v<T, FInt8Property>)
                    *GetPropertyPtr<int8>(writeBase, p->Offset) = static_cast<int8>(std::stoi(valueStr));
                else if constexpr (std::is_same_v<T, FInt16Property>)
                    *GetPropertyPtr<int16>(writeBase, p->Offset) = static_cast<int16>(std::stoi(valueStr));
                else if constexpr (std::is_same_v<T, FUInt16Property>)
                    *GetPropertyPtr<uint16>(writeBase, p->Offset) = static_cast<uint16>(std::stoul(valueStr));
                else if constexpr (std::is_same_v<T, FUInt32Property>)
                    *GetPropertyPtr<uint32>(writeBase, p->Offset) = static_cast<uint32>(std::stoul(valueStr));
                else if constexpr (std::is_same_v<T, FUInt64Property>)
                    *GetPropertyPtr<uint64>(writeBase, p->Offset) = std::stoull(valueStr);
                else if constexpr (std::is_same_v<T, FStrProperty>)
                    // Live object: assign (frees old buffer + copies) — never placement-new.
                    *GetPropertyPtr<UC::FString>(writeBase, p->Offset) = UC::FString(StringLib::ToWide(valueStr).c_str());
                else if constexpr (std::is_same_v<T, FNameProperty>)
                    // FName is POD (index pair) — plain assignment is correct for a live object.
                    *GetPropertyPtr<FName>(writeBase, p->Offset) = FName(StringLib::ToWide(valueStr).c_str());
                else if constexpr (std::is_same_v<T, FTextProperty>)
                    // FText is a refcounted handle, not a string — build a fresh one through the
                    // engine's Conv_StringToText UFunction (runs via ProcessEvent, so this must be
                    // on the game thread) and overwrite the slot. The previous FTextData's
                    // shared-ref leaks by one (the SDK FText has no operator= to release it), which
                    // is acceptable for a manual editor and far safer than mutating the shared,
                    // immutable TextSource in place.
                    *GetPropertyPtr<FText>(writeBase, p->Offset) =
                        UKismetTextLibrary::Conv_StringToText(ToFString(valueStr));
                else if constexpr (std::is_same_v<T, FEnumProperty>)
                {
                    // Numeric write into the enum's underlying integer slot.
                    const int64 val = std::stoll(valueStr);
                    uint8* slot = GetPropertyPtr<uint8>(writeBase, p->Offset);
                    switch (p->UnderlayingProperty ? p->UnderlayingProperty->ElementSize : 1)
                    {
                    case 1: *reinterpret_cast<uint8*> (slot) = static_cast<uint8> (val); break;
                    case 2: *reinterpret_cast<uint16*>(slot) = static_cast<uint16>(val); break;
                    case 4: *reinterpret_cast<uint32*>(slot) = static_cast<uint32>(val); break;
                    case 8: *reinterpret_cast<uint64*>(slot) = static_cast<uint64>(val); break;
                    }
                }
                else if constexpr (std::is_same_v<T, FStructProperty>)
                {
                    // POD math structs — direct assignment into the live layout.
                    const std::string sname = p->Struct ? p->Struct->GetName() : "";
                    if (sname.rfind("Vector", 0) == 0)
                    {
                        float x = 0, y = 0, z = 0;
                        sscanf_s(valueStr.c_str(), "%f,%f,%f", &x, &y, &z);
                        *GetPropertyPtr<SDK::FVector>(writeBase, p->Offset) = { x, y, z };
                    }
                    else if (sname == "Rotator")
                    {
                        float pitch = 0, yaw = 0, roll = 0;
                        sscanf_s(valueStr.c_str(), "%f,%f,%f", &pitch, &yaw, &roll);
                        *GetPropertyPtr<SDK::FRotator>(writeBase, p->Offset) = { pitch, yaw, roll };
                    }
                    else warn("[prop] struct '{}' not editable", sname);
                }
                else warn("[prop] set not supported for this property type");
                info("[prop] Set '{}.{}{}' = {}", obj->GetName(), baseName,
                    hadIndex ? "[" + std::to_string(elementIndex) + "]" : "", valueStr);
            }
            catch (...) { warn("[prop] Failed to parse value '{}'", valueStr); }
        });
}

// ---------------------------------------------------------------
// WriteArrayElement
// ---------------------------------------------------------------

bool WriteArrayElement(UObject* obj, uintptr_t arrayAddr, FArrayProperty* arrayProp,
                       int32 index, const std::string& valueStr)
{
    if (!arrayProp || !arrayProp->InnerProperty || index < 0 || !arrayAddr) return false;
    FProperty* inner = arrayProp->InnerProperty;
    const int32 es = inner->ElementSize;
    if (es <= 0) return false;

    // Reflected TArray runtime layout (FScriptArray). Re-read live on the game thread.
    struct FScriptArray { void* Data; int32 Num; int32 Max; };
    auto* a = reinterpret_cast<FScriptArray*>(arrayAddr);

    const int32 need = index + 1;   // need == Num+1 when appending
    if (need > a->Num)
    {
        // New slots are zero-filled, which is a valid default only for ZeroConstructor types.
        const auto pflags = static_cast<EPropertyFlags>(inner->PropertyFlags);
        if (!(pflags & EPropertyFlags::ZeroConstructor))
        {
            warn("[prop] cannot grow array of non-zero-constructible '{}'", GetTypeName(inner));
            return false;
        }
        if (need > a->Max)
        {
            // Same FMemory the array's own TArray uses (UERealloc -> GMalloc), so the engine
            // can later Realloc/Free this buffer with no cross-allocator mismatch.
            const uint32 align = es >= 16 ? 16u : 8u;
            void* nd = UC::Internal::UERealloc(a->Data, static_cast<uint64>(need) * es, align);
            if (!nd) { warn("[prop] array grow failed (OOM)"); return false; }
            a->Data = nd;
            a->Max  = need;
        }
        std::memset(static_cast<uint8*>(a->Data) + static_cast<uint64>(a->Num) * es, 0,
                    static_cast<uint64>(need - a->Num) * es);
        a->Num = need;
    }

    const uintptr_t elem = reinterpret_cast<uintptr_t>(a->Data) + static_cast<uint64>(index) * es;
    WriteProperty(obj, inner, elem, inner, valueStr, arrayProp->Name.ToString(), true, index);
    return true;
}

// ---------------------------------------------------------------
// ComputeParmsSize / WriteParam
// ---------------------------------------------------------------

int32 ComputeParmsSize(UFunction* Func)
{
    // Cover the WHOLE parameter block, INCLUDING the return parm: ProcessEvent writes the
    // return value at its offset, so excluding it (as this used to) under-sizes the buffer and
    // ProcessEvent scribbles past the end → crash for any function that returns a value.
    int32 size = 0;
    for (auto* field : FFieldRange(Func->ChildProperties))
    {
        if (!FieldCast::IsA<FProperty>(field)) continue;
        auto* Prop = static_cast<FProperty*>(field);
        auto  pf = static_cast<EPropertyFlags>(Prop->PropertyFlags);
        if (!(pf & EPropertyFlags::Parm)) continue;
        int32 end = Prop->Offset + Prop->ElementSize;
        if (end > size) size = end;
    }
    return (size + 7) & ~7;
}

bool WriteParam(FProperty* Prop, const std::string& token, uint8_t* parms)
{
    bool handled = false;
    FieldCast::Visit(Prop, [&]<typename T>(T* p)
    {
        auto base = reinterpret_cast<uintptr_t>(parms);
        if constexpr (std::is_same_v<T, FBoolProperty>)
        {
            bool v = (token == "1" || token == "true");
            if (p->FieldMask == 0xFF) *GetPropertyPtr<bool>(base, p->Offset) = v;
            else { uint8_t* byte = GetPropertyPtr<uint8_t>(base, p->Offset + p->ByteOffset); if (v) *byte |= p->FieldMask; else *byte &= ~p->FieldMask; }
            handled = true;
        }
        else if constexpr (std::is_same_v<T, FInt8Property>)   { *GetPropertyPtr<int8_t>  (base, p->Offset) = static_cast<int8_t>  (SafeStoll(token));  handled = true; }
        else if constexpr (std::is_same_v<T, FInt16Property>)  { *GetPropertyPtr<int16_t> (base, p->Offset) = static_cast<int16_t> (SafeStoll(token));  handled = true; }
        else if constexpr (std::is_same_v<T, FIntProperty>)    { *GetPropertyPtr<int32_t> (base, p->Offset) = static_cast<int32_t> (SafeStoll(token));  handled = true; }
        else if constexpr (std::is_same_v<T, FInt64Property>)  { *GetPropertyPtr<int64_t> (base, p->Offset) = SafeStoll(token);                         handled = true; }
        else if constexpr (std::is_same_v<T, FByteProperty>)   { *GetPropertyPtr<uint8_t> (base, p->Offset) = static_cast<uint8_t> (SafeStoull(token)); handled = true; }
        else if constexpr (std::is_same_v<T, FUInt16Property>) { *GetPropertyPtr<uint16_t>(base, p->Offset) = static_cast<uint16_t>(SafeStoull(token)); handled = true; }
        else if constexpr (std::is_same_v<T, FUInt32Property>) { *GetPropertyPtr<uint32_t>(base, p->Offset) = static_cast<uint32_t>(SafeStoull(token)); handled = true; }
        else if constexpr (std::is_same_v<T, FUInt64Property>) { *GetPropertyPtr<uint64_t>(base, p->Offset) = SafeStoull(token);                        handled = true; }
        else if constexpr (std::is_same_v<T, FFloatProperty>)  { *GetPropertyPtr<float>  (base, p->Offset) = SafeStof(token);                          handled = true; }
        else if constexpr (std::is_same_v<T, FDoubleProperty>) { *GetPropertyPtr<double>  (base, p->Offset) = SafeStod(token);                          handled = true; }
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
                default: warn("[cmd:call] unexpected enum backing size {}", p->UnderlayingProperty->ElementSize);
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
            new (GetPropertyPtr<FName>(base, p->Offset)) FName(StringLib::ToWide(token).c_str()); handled = true;
        }
        else if constexpr (std::is_same_v<T, FStructProperty>)
        {
            std::string sname = p->Struct->GetName();
            if (sname == "Vector" || sname == "Vector_NetQuantize" || sname == "Vector_NetQuantize10" ||
                sname == "Vector_NetQuantize100" || sname == "Vector_NetQuantizeNormal")
            {
                float x = 0, y = 0, z = 0;
                sscanf_s(token.c_str(), "%f,%f,%f", &x, &y, &z);
                // Raw property memory layout — use SDK type directly (matches ::FVector layout).
                *GetPropertyPtr<SDK::FVector>(base, p->Offset) = { x, y, z };
            }
            else if (sname == "Rotator")
            {
                float pitch = 0, yaw = 0, roll = 0;
                sscanf_s(token.c_str(), "%f,%f,%f", &pitch, &yaw, &roll);
                // Raw property memory layout — use SDK type directly (matches ::FRotator layout).
                *GetPropertyPtr<SDK::FRotator>(base, p->Offset) = { pitch, yaw, roll };
            }
            else if (sname == "Transform")
            {
                float rx = 0, ry = 0, rz = 0, rw = 1, tx = 0, ty = 0, tz = 0, sx = 1, sy = 1, sz = 1;
                sscanf_s(token.c_str(), "%f,%f,%f,%f %f,%f,%f %f,%f,%f", &rx, &ry, &rz, &rw, &tx, &ty, &tz, &sx, &sy, &sz);
                // Use SDK::FTransform here: direct member access (Rotation/Translation/Scale3D).
                // Our ::FTransform wrapper uses an sdk member + accessors, not direct fields.
                auto* t = GetPropertyPtr<SDK::FTransform>(base, p->Offset);
                t->Rotation = { rx, ry, rz, rw }; t->Translation = { tx, ty, tz }; t->Scale3D = { sx, sy, sz };
            }
            else warn("[cmd:call] struct '{}' not handled for '{}', leaving zeroed.", sname, p->Name.ToString());
            handled = true;
        }
        else if constexpr (std::is_same_v<T, FObjectProperty>     || std::is_same_v<T, FObjectPropertyBase> ||
                           std::is_same_v<T, FClassProperty>      || std::is_same_v<T, FWeakObjectProperty>)
        {
            UObject* found = nullptr;
            if (token != "null" && token != "none" && token != "0")
            {
                for (int32 i = 0; i < UObject::GObjects->NumElements; ++i)
                {
                    auto* obj = UObject::GObjects->GetByIndex(i);
                    if (obj && obj->GetName() == token) { found = obj; break; }
                }
                if (!found) warn("[cmd:call] object '{}' not found in GObjects.", token);
            }
            *GetPropertyPtr<UObject*>(base, p->Offset) = found;
            handled = true;
        }
        else warn("[cmd:call] unhandled property type for '{}', leaving zeroed.", p->Name.ToString());
    });
    return handled;
}

// ---------------------------------------------------------------
// Map helpers
// ---------------------------------------------------------------

bool GetMapLayout(UObject* obj, FMapProperty* mapProp, MapLayout& out)
{
    const uintptr_t mb = reinterpret_cast<uintptr_t>(obj) + mapProp->Offset;
    out.dataPtr      = *reinterpret_cast<const uintptr_t*>(mb);
    out.numAllocated = *reinterpret_cast<const int32*>(mb + 0x08);
    const uintptr_t bab = mb + 0x10;
    const int32* id  = reinterpret_cast<const int32*>(bab);
    const int32* sd  = *reinterpret_cast<int32* const*>(bab + 0x10);
    out.bitData      = sd ? reinterpret_cast<const uint32*>(sd) : reinterpret_cast<const uint32*>(id);
    out.pairStride   = (((mapProp->ValueProperty->Offset + mapProp->ValueProperty->ElementSize) + 7) & ~7) + 8;
    return out.dataPtr != 0 && out.numAllocated > 0;
}

bool KeyMatches(uintptr_t pairBase, FProperty* keyProp, const std::string& keyStr)
{
    return FieldCast::Visit(keyProp, [&](auto* p) -> bool
        {
            using T = std::remove_pointer_t<decltype(p)>;
            if constexpr (std::is_same_v<T, FStrProperty>)
            {
                auto* fstr = GetPropertyPtr<UC::FString>(pairBase, p->Offset);
                if (!fstr || !fstr->IsValid()) return keyStr.empty();
                std::wstring w(keyStr.begin(), keyStr.end());
                return wcsncmp(fstr->GetDataPtr(), w.c_str(), (std::max)(fstr->Num(), (int32)w.size())) == 0;
            }
            else if constexpr (std::is_same_v<T, FNameProperty>)
            {
                auto* fn = GetPropertyPtr<SDK::FName>(pairBase, p->Offset);
                if (!fn) return false;
                std::string ns = fn->ToString();
                auto ie = [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); };
                return ns.size() == keyStr.size() && std::equal(ns.begin(), ns.end(), keyStr.begin(), ie);
            }
            else if constexpr (std::is_same_v<T, FIntProperty>)    { try { return *GetPropertyPtr<int32> (pairBase, p->Offset) == std::stoi (keyStr); } catch (...) { return false; } }
            else if constexpr (std::is_same_v<T, FInt64Property>)   { try { return *GetPropertyPtr<int64> (pairBase, p->Offset) == std::stoll(keyStr); } catch (...) { return false; } }
            else if constexpr (std::is_same_v<T, FFloatProperty>)   { try { return std::abs(*GetPropertyPtr<float> (pairBase, p->Offset) - std::stof(keyStr)) < 1e-4f; } catch (...) { return false; } }
            else if constexpr (std::is_same_v<T, FDoubleProperty>)  { try { return std::abs(*GetPropertyPtr<double>(pairBase, p->Offset) - std::stod(keyStr)) < 1e-7;  } catch (...) { return false; } }
            else if constexpr (std::is_same_v<T, FBoolProperty>)    { return ReadBool(reinterpret_cast<UObject*>(pairBase), p) == (keyStr == "1" || keyStr == "true"); }
            else { warn("[prop] Key type not supported for key matching"); return false; }
        });
}

void ExecuteMapGet(UObject* obj, FMapProperty* mapProp, const std::string& keyStr)
{
    MapLayout l;
    if (!GetMapLayout(obj, mapProp, l)) { warn("[prop] Map '{}' is empty", mapProp->Name.ToString()); return; }
    for (int32 i = 0; i < l.numAllocated; ++i)
    {
        if (!l.bitData || !(l.bitData[i / 32] & (1u << (i % 32)))) continue;
        uintptr_t pb = l.dataPtr + i * l.pairStride;
        if (!KeyMatches(pb, mapProp->KeyProperty, keyStr)) continue;
        std::vector<bool> a{};
        PrintFieldValue(pb, mapProp->ValueProperty, a, true, "val");
        return;
    }
    warn("[prop] Key '{}' not found in map '{}'", keyStr, mapProp->Name.ToString());
}

void ExecuteMapGetByIndex(UObject* obj, FMapProperty* mapProp, int32 index)
{
    MapLayout l;
    if (!GetMapLayout(obj, mapProp, l)) { warn("[prop] Map empty"); return; }
    int32 live = 0;
    for (int32 i = 0; i < l.numAllocated; ++i)
    {
        if (!l.bitData || !(l.bitData[i / 32] & (1u << (i % 32)))) continue;
        if (live != index) { ++live; continue; }
        uintptr_t pb = l.dataPtr + i * l.pairStride;
        std::vector<bool> a{};
        PrintFieldValue(pb, mapProp->KeyProperty, a, false, "key");
        PrintFieldValue(pb, mapProp->ValueProperty, a, true, "val");
        return;
    }
    warn("[prop] Map index {} out of range", index);
}

void ExecuteMapSet(UObject* obj, FMapProperty* mapProp, const std::string& keyStr, const std::string& valueStr)
{
    MapLayout l;
    if (!GetMapLayout(obj, mapProp, l)) { warn("[prop] Map empty"); return; }
    for (int32 i = 0; i < l.numAllocated; ++i)
    {
        if (!l.bitData || !(l.bitData[i / 32] & (1u << (i % 32)))) continue;
        uintptr_t pb = l.dataPtr + i * l.pairStride;
        if (!KeyMatches(pb, mapProp->KeyProperty, keyStr)) continue;
        auto* fb = reinterpret_cast<UObject*>(pb);
        FieldCast::Visit(mapProp->ValueProperty, [&](auto* p)
        {
            using T = std::remove_pointer_t<decltype(p)>;
            try {
                if constexpr (std::is_same_v<T, FBoolProperty>)
                    WriteBool(fb, p, valueStr == "1" || valueStr == "true");
                else if constexpr (std::is_same_v<T, FIntProperty>)
                    *GetPropertyPtr<int32>(pb, p->Offset) = std::stoi(valueStr);
                else if constexpr (std::is_same_v<T, FFloatProperty>)
                    *GetPropertyPtr<float>(pb, p->Offset) = std::stof(valueStr);
                else if constexpr (std::is_same_v<T, FDoubleProperty>)
                    *GetPropertyPtr<double>(pb, p->Offset) = std::stod(valueStr);
                else if constexpr (std::is_same_v<T, FInt64Property>)
                    *GetPropertyPtr<int64>(pb, p->Offset) = std::stoll(valueStr);
                else if constexpr (std::is_same_v<T, FByteProperty>)
                    *GetPropertyPtr<uint8>(pb, p->Offset) = static_cast<uint8>(std::stoi(valueStr));
                else if constexpr (std::is_same_v<T, FStrProperty>)
                    *GetPropertyPtr<UC::FString>(pb, p->Offset) = UC::FString(StringLib::ToWide(valueStr).c_str());
                else warn("[prop] set not supported for value type '{}'", p->ClassPrivate->Name.ToString());
                info("[prop] Set map '{}'{{{}}} = {}", mapProp->Name.ToString(), keyStr, valueStr);
            }
            catch (...) { warn("[prop] Failed to parse value '{}'", valueStr); }
        });
        return;
    }
    warn("[prop] Key '{}' not found", keyStr);
}

void ExecuteMapSetByIndex(UObject* obj, FMapProperty* mapProp, int32 index, const std::string& valueStr)
{
    MapLayout l;
    if (!GetMapLayout(obj, mapProp, l)) { warn("[prop] Map empty"); return; }
    int32 live = 0;
    for (int32 i = 0; i < l.numAllocated; ++i)
    {
        if (!l.bitData || !(l.bitData[i / 32] & (1u << (i % 32)))) continue;
        if (live != index) { ++live; continue; }
        uintptr_t pb = l.dataPtr + i * l.pairStride;
        WriteProperty(obj, mapProp, pb, mapProp->ValueProperty, valueStr, mapProp->Name.ToString(), true, index);
        return;
    }
    warn("[prop] Map index {} out of range", index);
}

// ---------------------------------------------------------------
// ExecuteOnTarget / DispatchCommand
// ---------------------------------------------------------------

void Dump(UObject* target, UClass* outerBase)
{
    // If this is an actor component, show its owning actor before the properties.
    // GetComponentOwner returns nullptr for two reasons: not a component, or no
    // Outer set.  Distinguish by checking whether it is a component first.
    {
        static UClass* s_compCls = UActorComponent::StaticClass();
        if (s_compCls && target->IsA(s_compCls))
        {
            UObject* owner = target->Outer;
            if (owner)
                info("[prop] Component owner: '{}' ({})",
                     owner->GetName(), owner->Class ? owner->Class->GetName() : "?");
            else
                info("[prop] Component owner: <none>");
        }
    }

    DumpItemProperties(target, outerBase);
}

void ExecuteOnTarget(UObject* obj, const std::string& action,
    const std::string& propName, const std::string& valueStr)
{
    if (action == "dump") { Dump(obj); return; }

    std::string baseName = propName;
    bool  hadIndex = false; int32 elementIndex = -1;
    bool  hadKey   = false; std::string mapKey;

    if (const auto lb = propName.rfind('['); lb != std::string::npos)
    {
        const auto rb = propName.rfind(']');
        if (rb != std::string::npos && rb > lb)
        {
            try {
                int32 p = std::stoi(propName.substr(lb + 1, rb - lb - 1));
                if (p < 0) { warn("[prop] Negative index"); return; }
                elementIndex = p; hadIndex = true; baseName = propName.substr(0, lb);
            }
            catch (...) { warn("[prop] Invalid index in '{}'", propName); return; }
        }
    }
    else if (const auto lb2 = propName.rfind('{'); lb2 != std::string::npos)
    {
        const auto rb2 = propName.rfind('}');
        if (rb2 != std::string::npos && rb2 > lb2) { mapKey = propName.substr(lb2 + 1, rb2 - lb2 - 1); hadKey = true; baseName = propName.substr(0, lb2); }
        else { warn("[prop] Unmatched '{{' in '{}'", propName); return; }
    }

    if (action == "list")
    {
        auto props = FindAllProps(obj, baseName, true);
        info("[prop] {} properties matching '{}':", props.size(), baseName);
        std::vector<bool> a{};
        for (size_t i = 0; i < props.size(); ++i)
            PrintFieldValue(reinterpret_cast<uintptr_t>(obj), props[i], a, i == props.size() - 1);
        return;
    }

    if (action == "get")
    {
        auto* prop = FindProp(obj, baseName, false);
        if (!prop) { warn("[prop] Property '{}' not found", baseName); return; }
        if (hadKey) { auto* mp = FieldCast::Cast<FMapProperty>(prop); if (!mp) { warn("[prop] '{}' not TMap", baseName); return; } ExecuteMapGet(obj, mp, mapKey); return; }
        if (hadIndex)
        {
            auto* ap = FieldCast::Cast<FArrayProperty>(prop);
            if (!ap) { auto* mp = FieldCast::Cast<FMapProperty>(prop); if (!mp) { warn("[prop] '{}' not TArray/TMap", baseName); return; } ExecuteMapGetByIndex(obj, mp, elementIndex); return; }
            const auto& arr = *GetPropertyPtr<UC::TArray<uint8>>(obj, ap->Offset);
            if (!arr.IsValid() || elementIndex >= arr.Num()) { warn("[prop] Index {} out of range", elementIndex); return; }
            uintptr_t eb = reinterpret_cast<uintptr_t>(arr.GetDataPtr()) + elementIndex * ap->InnerProperty->ElementSize;
            std::vector<bool> a{};
            PrintFieldValue(eb, ap->InnerProperty, a, true);
            return;
        }
        PrintFieldValue(obj, prop, {}, true);
        return;
    }

    if (action == "set")
    {
        auto* prop = FindProp(obj, baseName, false);
        if (!prop) { warn("[prop] Property '{}' not found", baseName); return; }
        if (hadKey) { auto* mp = FieldCast::Cast<FMapProperty>(prop); if (!mp) { warn("[prop] '{}' not TMap", baseName); return; } ExecuteMapSet(obj, mp, mapKey, valueStr); return; }
        uintptr_t wb = reinterpret_cast<uintptr_t>(obj); FProperty* wp = prop;
        if (hadIndex)
        {
            auto* ap = FieldCast::Cast<FArrayProperty>(prop);
            if (!ap) { auto* mp = FieldCast::Cast<FMapProperty>(prop); if (!mp) { warn("[prop] '{}' not TArray/TMap", baseName); return; } ExecuteMapSetByIndex(obj, mp, elementIndex, valueStr); return; }
            const auto& arr = *GetPropertyPtr<UC::TArray<uint8>>(obj, ap->Offset);
            if (!arr.IsValid() || elementIndex >= arr.Num()) { warn("[prop] Index {} out of range", elementIndex); return; }
            wb = reinterpret_cast<uintptr_t>(arr.GetDataPtr()) + elementIndex * ap->InnerProperty->ElementSize;
            wp = ap->InnerProperty;
        }
        WriteProperty(obj, prop, wb, wp, valueStr, baseName, hadIndex, elementIndex);
    }
}

void DispatchCommand(const CommandContext& ctx)
{
    if (ctx.ArgCount() < 2) { info("[prop] usage: prop <cdo|obj> <n> <dump|get|set|list> [prop] [value] [fuzzy]"); return; }

    if (ctx.Arg(1) == "pick")
    {
        if (!g_Pending) { warn("[prop] No pending selection."); return; }
        int idx = -1;
        try { idx = std::stoi(ctx.Arg(2)); } catch (...) {}
        if (idx < 0 || idx >= (int)g_Pending->candidates.size()) { warn("[prop] Index {} out of range", idx); return; }
        UObject* chosen = g_Pending->candidates[idx];
        if (!IsValidRaw(chosen)) { warn("[prop] Candidate no longer valid."); g_Pending.reset(); return; }
        info("[prop] Selected: '{}'", chosen->GetName());
        ExecuteOnTarget(chosen, g_Pending->pendingAction, g_Pending->pendingProp, g_Pending->pendingValue);
        g_Pending.reset();
        return;
    }

    if (ctx.ArgCount() < 4) { info("[prop] usage: prop <cdo|obj> <n> <dump|get|set|list> [prop] [value] [fuzzy]"); return; }

    const std::string& kindStr = ctx.Arg(1), & name = ctx.Arg(2), & action = ctx.Arg(3);
    const bool hasPropSlot = (action != "dump");
    const std::string propName  = hasPropSlot ? ctx.Arg(4) : "";
    const std::string value     = hasPropSlot ? ctx.Arg(5) : "";
    const size_t flagStart      = hasPropSlot ? 6 : 4;

    bool fuzzy = false; std::string parentClassName;
    for (size_t i = flagStart; i < ctx.ArgCount(); ++i)
    {
        if (ctx.Arg(i) == "fuzzy") fuzzy = true;
        if (ctx.Arg(i) == "class" && i + 1 < ctx.ArgCount()) parentClassName = ctx.Arg(++i);
    }

    UClass* parentClass = nullptr;
    if (!parentClassName.empty())
    {
        parentClass = FindClass(parentClassName, false);
        if (!parentClass) { parentClass = FindClass(parentClassName, true); if (parentClass) info("[prop] Class '{}' fuzzy→'{}'", parentClassName, parentClass->GetName()); }
        if (!parentClass)
        {
            // Abort — silently dropping a missing class filter would dump every
            // UObject, which is almost certainly not what the user asked for.
            warn("[prop] Parent class '{}' not found — aborting.", parentClassName);
            return;
        }
    }

    bool isCDO = (kindStr == "cdo");
    auto candidates = isCDO ? FindAllCDOs(name, fuzzy) : FindAllInstances(name, fuzzy, parentClass);

    if (!propName.empty() && action != "dump")
    {
        std::string bp = propName;
        if (const auto lb = propName.rfind('[');  lb  != std::string::npos) bp = propName.substr(0, lb);
        else if (const auto lb2 = propName.rfind('{'); lb2 != std::string::npos) bp = propName.substr(0, lb2);
        candidates.erase(
            std::remove_if(candidates.begin(), candidates.end(),
                [&](UObject* o) { return FindProp(o, bp, false) == nullptr; }),
            candidates.end());
    }

    if (candidates.empty())
    {
        warn("[prop] No {} matching '{}'{}{}", isCDO ? "CDOs" : "instances", name,
            fuzzy ? "" : " (try 'fuzzy')",
            propName.empty() ? "" : " with property '" + propName + "'");
        return;
    }

    if (candidates.size() == 1)
    {
        info("[prop] Resolved: '{}'", candidates[0]->GetName());
        ExecuteOnTarget(candidates[0], action, propName, value);
        return;
    }

    info("[prop] {} candidates for '{}', use 'prop pick <index>':", candidates.size(), name);
    std::unordered_map<std::string, int> cnc;
    for (auto* o : candidates) if (o->Class) cnc[o->Class->GetName()]++;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        UClass* cls = candidates[i]->Class;
        bool ambiguous = cls && cnc[cls->GetName()] > 1;
        std::string label = ambiguous
            ? Kismet::Conv_SoftClassReferenceToString(Kismet::Conv_ClassToSoftClassReference(cls)).ToString()
            : (cls ? cls->GetName() : "?");
        // For components, append the owning actor so the user can tell them apart.
        std::string ownerSuffix;
        if (UObject* owner = GetComponentOwner(candidates[i]))
            ownerSuffix = " → " + owner->GetName();
        info("  [{}] {}{} ({})", i, candidates[i]->GetName(), ownerSuffix, label);
    }
    g_Pending = PendingSelection{ candidates, action, propName, value };
}

} // namespace PropertyInspector
