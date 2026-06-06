#include "Lib_Print.h"
#include "Lib_ObjectCast.h"
#include "Lib_PropertyAccess.h"
#include "Lib_Utils.h"
#include <algorithm>
#include <format>

using namespace SDK;  // file-local; no math types used in this TU

// =========================================================================
// Enum name resolution
// =========================================================================

namespace {
// Resolve an enum value to its human-readable display name.
//
// Strategy (first non-empty result wins):
//   1. UKismetNodeHelperLibrary::GetEnumeratorUserFriendlyName — reads UEnum metadata,
//      which stores the label typed in the Blueprint editor.  This is the correct path
//      for user-defined enums whose Names entries are mangled (e.g.
//      "UserDefinedEnum1::NewEnumerator0").  Only safe to call when elemSize == 1
//      because the function signature takes uint8.
//   2. Scan UEnum::Names, stripping the "ClassName::" prefix that UE always prepends
//      (e.g. "EFlareLaunchType::Arc" → "Arc").
//
// Pass elemSize = 1 for FByteProperty; pass UnderlayingProperty->ElementSize for
// FEnumProperty (which may be > 1 for non-byte underlying types).
std::string ResolveEnumName(const UEnum* e, int64 val, int32 elemSize)
{
    if (!e) return {};
    std::string name;

    if (elemSize == 1)
    {
        FString f = UKismetNodeHelperLibrary::GetEnumeratorUserFriendlyName(e, static_cast<uint8>(val));
        name = f.IsValid() ? f.ToString() : "";
    }

    if (name.empty())
    {
        for (int32 i = 0; i < e->Names.Num(); ++i)
        {
            if (e->Names[i].Second == val)
            {
                name = e->Names[i].First.ToString();
                if (const auto pos = name.rfind("::"); pos != std::string::npos)
                    name = name.substr(pos + 2);
                break;
            }
        }
    }
    return name;
}
} // anonymous namespace

// =========================================================================
// Class chain helpers
// =========================================================================

std::vector<UStruct*> BuildClassChain(UClass* Class, UClass* OuterBase)
{
    std::vector<UStruct*> chain;
    if (!Class) return chain;
    const bool useOuterBase = OuterBase && IsValidClass(OuterBase) && MathLib::ClassIsChildOf(Class, OuterBase);
    for (UStruct* current = Class; current; current = current->SuperStruct)
    {
        chain.push_back(current);
        UClass* asClass = ObjectCast::Cast<UClass>(current);
        if (!asClass) break;
        if (useOuterBase && MathLib::EqualEqual_ClassClass(asClass, OuterBase)) break;
        if (!useOuterBase && IsNativeClass(asClass)) break;
    }
    return chain;
}

void PrintClassHierarchy(const UClass* Class)
{
    for (const UStruct* c = Class; c; c = c->SuperStruct)
        info("{}", c->GetName());
}

// =========================================================================
// Detail
// =========================================================================

std::string Detail::Prefix(const std::vector<bool>& ancestry, bool isLast)
{
    std::string r = "║   ";
    for (bool open : ancestry) r += open ? "│   " : "    ";
    r += isLast ? "└── " : "├── ";
    return r;
}

std::string Detail::ChildPrefix(const std::vector<bool>& ancestry, bool open)
{
    std::string r = "║   ";
    for (bool a : ancestry) r += a ? "│   " : "    ";
    r += open ? "│   " : "    ";
    return r;
}

std::string Detail::ClassTag(const UClass* cls)
{
    if (IsNativeClass(cls) && IsBlueprintClass(cls)) return "native+bp";
    if (IsNativeClass(cls))                          return "native";
    if (IsBlueprintClass(cls))                       return "blueprint";
    return "unknown";
}

// =========================================================================
// PrintFieldValue
// =========================================================================

void PrintFieldValue(uintptr_t Base, FField* Field,
    const std::vector<bool>& ancestry, bool isLast,
    const std::string& labelOverride)
{
    const std::string pre = Detail::Prefix(ancestry, isLast);
    FieldCast::Visit(Field, [&](auto* prop)
        {
            using T = std::remove_pointer_t<decltype(prop)>;
            const std::string fname = labelOverride.empty() ? prop->Name.ToString() : labelOverride;
            auto* fakeBase = reinterpret_cast<UObject*>(Base);

            if constexpr (std::is_same_v<T, FIntProperty>)    info("{}{} = {}", pre, fname, *GetPropertyPtr<int32>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FInt8Property>)   info("{}{} = {}", pre, fname, *GetPropertyPtr<int8>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FInt16Property>)  info("{}{} = {}", pre, fname, *GetPropertyPtr<int16>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FInt64Property>)  info("{}{} = {}", pre, fname, *GetPropertyPtr<int64>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FUInt16Property>) info("{}{} = {}", pre, fname, *GetPropertyPtr<uint16>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FUInt32Property>) info("{}{} = {}", pre, fname, *GetPropertyPtr<uint32>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FUInt64Property>) info("{}{} = {}", pre, fname, *GetPropertyPtr<uint64>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FFloatProperty>)  info("{}{} = {:.4f}", pre, fname, *GetPropertyPtr<float>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FDoubleProperty>) info("{}{} = {:.6f}", pre, fname, *GetPropertyPtr<double>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FByteProperty>)
            {
                const uint8 byteVal = *GetPropertyPtr<uint8>(Base, prop->Offset);
                if (prop->Enum)
                {
                    const std::string enumStr = ResolveEnumName(prop->Enum, static_cast<int64>(byteVal), 1);
                    if (enumStr.empty())
                        info("{}{} = {}", pre, fname, byteVal);
                    else
                        info("{}{} = {} | {}", pre, fname, enumStr, byteVal);
                }
                else
                    info("{}{} = {}", pre, fname, byteVal);
            }
            else if constexpr (std::is_same_v<T, FBoolProperty>)
                info("{}{} = {}", pre, fname, ReadBool(fakeBase, prop));
            else if constexpr (std::is_same_v<T, FStrProperty>)
            {
                const auto& str = *GetPropertyPtr<UC::FString>(Base, prop->Offset);
                info("{}{} = \"{}\"", pre, fname, str.IsValid() ? str.ToString() : "<empty>");
            }
            else if constexpr (std::is_same_v<T, FNameProperty>)
                info("{}{} = {}", pre, fname, GetPropertyRef<FName>(fakeBase, prop->Offset).ToString());
            else if constexpr (std::is_same_v<T, FTextProperty>)
                info("{}{} = [FText]", pre, fname);
            else if constexpr (std::is_same_v<T, FEnumProperty>)
            {
                int64 enumVal = 0;
                memcpy(&enumVal, GetPropertyPtr<void>(Base, prop->Offset), prop->UnderlayingProperty->ElementSize);
                const std::string enumStr = ResolveEnumName(prop->Enum, enumVal, prop->UnderlayingProperty->ElementSize);
                if (enumStr.empty())
                    info("{}{} = {}", pre, fname, enumVal);
                else
                    info("{}{} = {} | {}", pre, fname, enumStr, enumVal);
            }
            else if constexpr (std::is_same_v<T, FClassProperty>)
            {
                auto* cls = *GetPropertyPtr<UClass*>(Base, prop->Offset);
                info("{}{} = {}", pre, fname, cls && IsValid(cls) ? cls->GetName() : "null");
            }
            else if constexpr (std::is_same_v<T, FObjectProperty> || std::is_same_v<T, FObjectPropertyBase>)
            {
                auto* obj = *GetPropertyPtr<UObject*>(Base, prop->Offset);
                if (!obj || !IsValidRaw(obj))
                {
                    info("{}{} = null", pre, fname);
                }
                else
                {
                    // Actor component: prefix with owner actor name
                    std::string display = obj->GetName();
                    if (ObjectCast::Cast<UActorComponent>(obj))
                    {
                        auto* outerActor = ObjectCast::Cast<AActor>(obj->Outer);
                        if (outerActor && IsValidRaw(outerActor))
                            display = std::format("{}.{}", outerActor->GetName(), obj->GetName());
                    }
                    info("{}{} = {} ({})", pre, fname, display, obj->Class->GetName());
                }
            }
            else if constexpr (std::is_same_v<T, FStructProperty>)
            {
                info("{}{} ({}):", pre, fname, prop->Struct->GetName());
                std::vector<FField*> children;
                for (auto* inner : FFieldRange(prop->Struct->ChildProperties)) children.push_back(inner);
                std::vector<bool> ca = ancestry; ca.push_back(!isLast);
                const uintptr_t sb = Base + prop->Offset;
                for (size_t i = 0; i < children.size(); ++i)
                    PrintFieldValue(sb, children[i], ca, i == children.size() - 1);
            }
            else if constexpr (std::is_same_v<T, FArrayProperty>)
            {
                const auto& arr = *GetPropertyPtr<UC::TArray<uint8>>(Base, prop->Offset);
                const int32 num = arr.Num(), maxShow = (std::min)(num, 8);
                info("{}{} [TArray {}/{}] <{}>", pre, fname, num, arr.Max(),
                    prop->InnerProperty->ClassPrivate->Name.ToString());
                if (arr.IsValid() && num > 0)
                {
                    const int32 es = prop->InnerProperty->ElementSize;
                    const uintptr_t db = reinterpret_cast<uintptr_t>(arr.GetDataPtr());
                    std::vector<bool> ca = ancestry; ca.push_back(!isLast);
                    for (int32 i = 0; i < maxShow; ++i)
                    {
                        const bool le = (i == maxShow - 1) && (num <= maxShow);
                        info("{}[{}]", Detail::Prefix(ca, le), i);
                        std::vector<bool> ea = ca; ea.push_back(!le);
                        PrintFieldValue(db + i * es, prop->InnerProperty, ea, true);
                    }
                    if (num > maxShow) info("{}... ({} more)", Detail::ChildPrefix(ancestry, !isLast), num - maxShow);
                }
            }
            else if constexpr (std::is_same_v<T, FSetProperty>)
            {
                const uintptr_t sb  = Base + prop->Offset;
                const uintptr_t dp  = *reinterpret_cast<const uintptr_t*>(sb);
                const int32     na  = *reinterpret_cast<const int32*>(sb + 0x08);
                const uintptr_t bab = sb + 0x10;
                const int32*    id  = reinterpret_cast<const int32*>(bab);
                const int32*    sd  = *reinterpret_cast<int32* const*>(bab + 0x10);
                const uint32*   bd  = sd ? reinterpret_cast<const uint32*>(sd) : reinterpret_cast<const uint32*>(id);
                const int32 es  = prop->ElementProperty->ElementSize + 8;
                int32 lc = 0;
                for (int32 i = 0; i < na; ++i) if (bd && (bd[i / 32] & (1u << (i % 32)))) ++lc;
                info("{}{} [TSet {}/{}] <{}>", pre, fname, lc, na, prop->ElementProperty->ClassPrivate->Name.ToString());
                if (!dp || na <= 0) return;
                std::vector<bool> ca = ancestry; ca.push_back(!isLast);
                const int32 ms = (std::min)(lc, 8); int32 shown = 0;
                for (int32 i = 0; i < na && shown < ms; ++i)
                {
                    if (!bd || !(bd[i / 32] & (1u << (i % 32)))) continue;
                    const bool le = (shown == ms - 1) && (lc <= ms);
                    info("{}[{}]", Detail::Prefix(ca, le), shown);
                    PrintFieldValue(dp + i * es, prop->ElementProperty, ca, true);
                    ++shown;
                }
                if (lc > ms) info("{}... ({} more)", Detail::ChildPrefix(ancestry, !isLast), lc - ms);
            }
            else if constexpr (std::is_same_v<T, FMapProperty>)
            {
                const uintptr_t mb  = Base + prop->Offset;
                const uintptr_t dp  = *reinterpret_cast<const uintptr_t*>(mb);
                const int32     na  = *reinterpret_cast<const int32*>(mb + 0x08);
                const uintptr_t bab = mb + 0x10;
                const int32*    id  = reinterpret_cast<const int32*>(bab);
                const int32*    sd  = *reinterpret_cast<int32* const*>(bab + 0x10);
                const uint32*   bd  = sd ? reinterpret_cast<const uint32*>(sd) : reinterpret_cast<const uint32*>(id);
                const int32 ps  = (((prop->ValueProperty->Offset + prop->ValueProperty->ElementSize) + 7) & ~7) + 8;
                int32 lc = 0;
                for (int32 i = 0; i < na; ++i) if (bd && (bd[i / 32] & (1u << (i % 32)))) ++lc;
                info("{}{} [TMap {}/{}] <{}, {}>", pre, fname, lc, na,
                    prop->KeyProperty->ClassPrivate->Name.ToString(),
                    prop->ValueProperty->ClassPrivate->Name.ToString());
                if (!dp || na <= 0) return;
                std::vector<bool> ca = ancestry; ca.push_back(!isLast);
                const int32 ms = (std::min)(lc, 8); int32 shown = 0;
                for (int32 i = 0; i < na && shown < ms; ++i)
                {
                    if (!bd || !(bd[i / 32] & (1u << (i % 32)))) continue;
                    const uintptr_t pb = dp + i * ps;
                    const bool le = (shown == ms - 1) && (lc <= ms);
                    info("{}[{}]", Detail::Prefix(ca, le), shown);
                    std::vector<bool> ea = ca; ea.push_back(!le);
                    PrintFieldValue(pb, prop->KeyProperty, ea, false, "key");
                    PrintFieldValue(pb, prop->ValueProperty, ea, true,  "val");
                    ++shown;
                }
                if (lc > ms) info("{}... ({} more)", Detail::ChildPrefix(ancestry, !isLast), lc - ms);
            }
            else if constexpr (std::is_base_of_v<FProperty, T>)
                info("{}{} @ +{:#x} ({} bytes) [{}]", pre, fname, prop->Offset, prop->ElementSize,
                    prop->ClassPrivate ? prop->ClassPrivate->Name.ToString() : "?");
        });
}

void PrintFieldValue(UObject* Base, FField* Field,
    const std::vector<bool>& ancestry, bool isLast,
    const std::string& labelOverride)
{
    PrintFieldValue(reinterpret_cast<uintptr_t>(Base), Field, ancestry, isLast, labelOverride);
}

// =========================================================================
// DumpItemProperties
// =========================================================================

template <bool bSort>
void DumpItemProperties(UObject* Item, UClass* OuterBase)
{
    // Use IsValidRaw (flag check on EObjectFlags) rather than Kismet::IsValid:
    // the latter goes through ProcessEvent and rejects on EInternalObjectFlags
    // (Garbage / Unreachable) that the engine sometimes sets on actors which
    // are still being controlled by the player. For a property dump the only
    // thing that actually matters is that the memory is safe to dereference.
    if (!Item)           { warn("[dump] null target"); return; }
    if (!IsValidRaw(Item)){ warn("[dump] '{}' is flagged BeginDestroyed/MirroredGarbage — skipping", Item->GetName()); return; }
    if (!Item->Class)    { warn("[dump] '{}' has null Class — skipping", Item->GetName()); return; }

    const auto chain = BuildClassChain(Item->Class, OuterBase);
    info("╔══ {} ({})", Item->GetName(), Item->Class->GetName());

    for (size_t i = 0; i < chain.size(); ++i)
    {
        UStruct* level   = chain[i];
        UClass*  asClass = ObjectCast::Cast<UClass>(level);
        if (!asClass) continue;  // chain tail may be a non-UClass UStruct — ClassTag reads UClass flags

        info("╠═ [{}] {}", Detail::ClassTag(asClass), level->GetName());

        if (!level->ChildProperties) {
            info("║   (no properties)");
            continue;
        }

        std::vector<FField*> fields;
        for (FField* field : FFieldRange(level->ChildProperties))
            fields.push_back(field);

        if constexpr (bSort)
        {
            std::sort(fields.begin(), fields.end(), [](FField* A, FField* B) {
                auto TypeA = A->ClassPrivate->Name.ToString();
                auto TypeB = B->ClassPrivate->Name.ToString();
                if (TypeA != TypeB) return TypeA < TypeB;
                return A->Name.ToString() < B->Name.ToString();
            });
        }

        const std::vector<bool> root{};
        for (size_t j = 0; j < fields.size(); ++j)
            PrintFieldValue(reinterpret_cast<uintptr_t>(Item), fields[j], root, j == fields.size() - 1);
    }
    info("╚══════════════════════════");
}

// Explicit instantiations — satisfies every extern template declaration in the header.
// bool has exactly two values so this is exhaustive.
template void DumpItemProperties<false>(UObject*, UClass*);
template void DumpItemProperties<true> (UObject*, UClass*);

// =========================================================================
// DumpItemPropertiesSorted
// =========================================================================

void DumpItemPropertiesSorted(UObject* Item, UClass* OuterBase)
{
    if (!Item || !IsValidRaw(Item)) return;

    auto chain = BuildClassChain(Item->Class, OuterBase);

    info("╔══ {} ({})", Item->GetName(), Item->Class->GetName());

    for (auto* structlevel : chain)
    {
        auto level = ObjectCast::Cast<UClass>(structlevel);
        if (!level) continue;
        info("╠═ [{}] {}", Detail::ClassTag(level), level->GetName());

        if (!level->ChildProperties) {
            info("║   (no properties)");
            continue;
        }

        struct PropInfo { std::string Name; std::string Type; };
        std::vector<PropInfo> propList;

        for (FField* field : FFieldRange(level->ChildProperties))
        {
            auto* prop = FieldCast::Cast<FProperty>(field);
            if (!prop) continue;

            PropInfo info;
            info.Name = prop->Name.ToString();
            info.Type = prop->ClassPrivate ? prop->ClassPrivate->Name.ToString() : "UnknownType";

            if (auto* sp = FieldCast::Cast<FStructProperty>(prop))
                info.Type += std::format("<{}>", sp->Struct->GetName());
            else if (auto* op = FieldCast::Cast<FObjectPropertyBase>(prop))
                info.Type += std::format("<{}>", op->PropertyClass->GetName());
            else if (auto* ap = FieldCast::Cast<FArrayProperty>(prop))
                info.Type += std::format("<{}>", ap->InnerProperty->ClassPrivate->Name.ToString());

            propList.push_back(info);
        }

        std::sort(propList.begin(), propList.end(), [](const PropInfo& a, const PropInfo& b) {
            if (a.Type != b.Type) return a.Type < b.Type;
            return a.Name < b.Name;
        });

        for (const auto& p : propList)
            info("║   {:<35} : {}", p.Name, p.Type);
    }
    info("╚══════════════════════════");
}

// =========================================================================
// GetFieldValueAsString
// =========================================================================

std::string GetFieldValueAsString(uintptr_t Base, FField* Field)
{
    std::string result;
    FieldCast::Visit(Field, [&](auto* prop)
        {
            using T = std::remove_pointer_t<decltype(prop)>;
            auto* fakeBase = reinterpret_cast<UObject*>(Base);

            if constexpr (std::is_same_v<T, FIntProperty>)
                result = std::to_string(*GetPropertyPtr<int32>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FInt8Property>)
                result = std::to_string(*GetPropertyPtr<int8>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FInt16Property>)
                result = std::to_string(*GetPropertyPtr<int16>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FInt64Property>)
                result = std::to_string(*GetPropertyPtr<int64>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FUInt16Property>)
                result = std::to_string(*GetPropertyPtr<uint16>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FUInt32Property>)
                result = std::to_string(*GetPropertyPtr<uint32>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FUInt64Property>)
                result = std::to_string(*GetPropertyPtr<uint64>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FFloatProperty>)
                result = std::format("{:.6g}", *GetPropertyPtr<float>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FDoubleProperty>)
                result = std::format("{:.6g}", *GetPropertyPtr<double>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FByteProperty>)
            {
                const uint8 byteVal = *GetPropertyPtr<uint8>(Base, prop->Offset);
                if (prop->Enum)
                {
                    const std::string enumStr = ResolveEnumName(prop->Enum, static_cast<int64>(byteVal), 1);
                    result = enumStr.empty()
                        ? std::to_string(byteVal)
                        : std::format("{} | {}", enumStr, byteVal);
                }
                else
                    result = std::to_string(byteVal);
            }
            else if constexpr (std::is_same_v<T, FBoolProperty>)
                result = ReadBool(fakeBase, prop) ? "true" : "false";
            else if constexpr (std::is_same_v<T, FStrProperty>)
            {
                const auto& str = *GetPropertyPtr<UC::FString>(Base, prop->Offset);
                result = str.IsValid() ? str.ToString() : "";
            }
            else if constexpr (std::is_same_v<T, FNameProperty>)
                result = GetPropertyRef<FName>(fakeBase, prop->Offset).ToString();
            else if constexpr (std::is_same_v<T, FEnumProperty>)
            {
                int64 enumVal = 0;
                memcpy(&enumVal, GetPropertyPtr<void>(Base, prop->Offset), prop->UnderlayingProperty->ElementSize);
                const std::string enumStr = ResolveEnumName(prop->Enum, enumVal, prop->UnderlayingProperty->ElementSize);
                result = enumStr.empty()
                    ? std::to_string(enumVal)
                    : std::format("{} | {}", enumStr, enumVal);
            }
            else if constexpr (std::is_same_v<T, FClassProperty>)
            {
                auto* cls = *GetPropertyPtr<UClass*>(Base, prop->Offset);
                result = cls && IsValid(cls) ? cls->GetName() : "null";
            }
            else if constexpr (std::is_same_v<T, FObjectProperty> || std::is_same_v<T, FObjectPropertyBase>)
            {
                auto* obj = *GetPropertyPtr<UObject*>(Base, prop->Offset);
                if (!obj || !IsValidRaw(obj))
                {
                    result = "null";
                }
                else if (ObjectCast::Cast<UActorComponent>(obj))
                {
                    // Actor component: "OwnerActor.ComponentName"
                    auto* outerActor = ObjectCast::Cast<AActor>(obj->Outer);
                    result = (outerActor && IsValidRaw(outerActor))
                        ? std::format("{}.{}", outerActor->GetName(), obj->GetName())
                        : obj->GetName();
                }
                else
                    result = obj->GetName();
            }
            else if constexpr (std::is_same_v<T, FStructProperty>)
            {
                const std::string sn = prop->Struct ? prop->Struct->GetName() : "";
                if (sn == "Rotator")
                {
                    const auto& r = *GetPropertyPtr<FRotator>(Base, prop->Offset);
                    result = std::format("P={:.2f} Y={:.2f} R={:.2f}", r.Pitch, r.Yaw, r.Roll);
                }
                else if (sn == "Vector"              || sn == "Vector_NetQuantize"     ||
                         sn == "Vector_NetQuantize10" || sn == "Vector_NetQuantize100"  ||
                         sn == "Vector_NetQuantizeNormal")
                {
                    const auto& v = *GetPropertyPtr<FVector>(Base, prop->Offset);
                    result = std::format("({:.2f}, {:.2f}, {:.2f})", v.X, v.Y, v.Z);
                }
                else
                {
                    // Generic struct: enumerate child properties up to a short cap.
                    // Gives readable inline output for any unknown struct type
                    // (FTransform, FQuat, FLinearColor, FGameplayTag, etc.).
                    const uintptr_t sb = Base + prop->Offset;
                    std::string inner;
                    int32 shown = 0;
                    constexpr int32 kMaxFields = 8;
                    if (prop->Struct && prop->Struct->ChildProperties)
                    {
                        for (FField* f : FFieldRange(prop->Struct->ChildProperties))
                        {
                            if (!FieldCast::IsA<FProperty>(f)) continue;
                            if (shown >= kMaxFields) { inner += ", ..."; break; }
                            if (shown > 0) inner += ", ";
                            inner += static_cast<FProperty*>(f)->Name.ToString()
                                   + '=' + GetFieldValueAsString(sb, f);
                            ++shown;
                        }
                    }
                    result = shown > 0 ? '{' + inner + '}' : std::format("<struct:{}>", sn);
                }
            }
            else if constexpr (std::is_same_v<T, FWeakObjectProperty> ||
                               std::is_same_v<T, FLazyObjectProperty>)
            {
                // TWeakObjectPtr / TLazyObjectPtr — resolve to the live object (or null
                // if the referent has been GC'd). FWeakObjectPtr::Get() does the
                // index/serial validation, so a stale handle safely yields null.
                UObject* obj = GetPropertyPtr<FWeakObjectPtr>(Base, prop->Offset)->Get();
                result = (obj && IsValidRaw(obj)) ? obj->GetName() : "null";
            }
            else if constexpr (std::is_same_v<T, FArrayProperty>)
            {
                // Inline TArray rendering for the netlog param string. Elements are laid
                // out contiguously; recurse into InnerProperty (offset 0) per element.
                const auto& arr = *GetPropertyPtr<UC::TArray<uint8>>(Base, prop->Offset);
                const int32 num = arr.Num();
                if (!prop->InnerProperty || !arr.IsValid() || num <= 0)
                    result = "[]";
                else
                {
                    constexpr int32 kMaxShow = 8;
                    const int32     maxShow  = (std::min)(num, kMaxShow);
                    const int32     es       = prop->InnerProperty->ElementSize;
                    const uintptr_t db       = reinterpret_cast<uintptr_t>(arr.GetDataPtr());
                    std::string inner;
                    for (int32 i = 0; i < maxShow; ++i)
                    {
                        if (i > 0) inner += ", ";
                        inner += GetFieldValueAsString(db + static_cast<uintptr_t>(i) * es,
                                                       prop->InnerProperty);
                    }
                    if (num > maxShow) inner += std::format(", ... ({} more)", num - maxShow);
                    result = '[' + inner + ']';
                }
            }
            else
                result = "<unsupported>";
        });
    return result;
}

// =========================================================================
// Flag diagnostics
// =========================================================================

std::string DumpFunctionFlags(uint32_t flags)
{
    using F = SDK::EFunctionFlags;
    std::string out;
    auto add = [&](F f, const char* name) {
        if (flags & static_cast<uint32_t>(f)) {
            if (!out.empty()) out += '|';
            out += name;
        }
    };
    add(F::Final,                  "Final");
    add(F::RequiredAPI,            "ReqAPI");
    add(F::BlueprintAuthorityOnly, "BPAuthOnly");
    add(F::BlueprintCosmetic,      "BPCosmetic");
    add(F::Net,                    "Net");
    add(F::NetReliable,            "NetReliable");
    add(F::NetRequest,             "NetRequest");
    add(F::Exec,                   "Exec");
    add(F::Native,                 "Native");
    add(F::Event,                  "Event");
    add(F::NetResponse,            "NetResponse");
    add(F::Static,                 "Static");
    add(F::NetMulticast,           "NetMulticast");
    add(F::UbergraphFunction,      "UbergraphFn");
    add(F::MulticastDelegate,      "MCDelegate");
    add(F::Public,                 "Public");
    add(F::Private,                "Private");
    add(F::Protected,              "Protected");
    add(F::Delegate,               "Delegate");
    add(F::NetServer,              "NetServer");
    add(F::HasOutParms,            "HasOutParms");
    add(F::HasDefaults,            "HasDefaults");
    add(F::NetClient,              "NetClient");
    add(F::DLLImport,              "DLLImport");
    add(F::BlueprintCallable,      "BPCallable");
    add(F::BlueprintEvent,         "BPEvent");
    add(F::BlueprintPure,          "BPPure");
    add(F::EditorOnly,             "EditorOnly");
    add(F::Const,                  "Const");
    add(F::NetValidate,            "NetValidate");
    if (out.empty()) out = "None";
    return out;
}

std::string DumpPropertyFlags(SDK::EPropertyFlags flags)
{
    if (flags == EPropertyFlags::None) return "None";
    uint64_t mask = static_cast<uint64_t>(flags);
    struct FFlagMap { EPropertyFlags Flag; const char* Name; };
    static const FFlagMap FlagEntries[] = {
        { EPropertyFlags::Edit,                           "Edit" },
        { EPropertyFlags::ConstParm,                      "ConstParm" },
        { EPropertyFlags::BlueprintVisible,               "BlueprintVisible" },
        { EPropertyFlags::ExportObject,                   "ExportObject" },
        { EPropertyFlags::BlueprintReadOnly,              "BlueprintReadOnly" },
        { EPropertyFlags::Net,                            "Net" },
        { EPropertyFlags::EditFixedSize,                  "EditFixedSize" },
        { EPropertyFlags::Parm,                           "Parm" },
        { EPropertyFlags::OutParm,                        "OutParm" },
        { EPropertyFlags::ZeroConstructor,                "ZeroConstructor" },
        { EPropertyFlags::ReturnParm,                     "ReturnParm" },
        { EPropertyFlags::DisableEditOnTemplate,          "DisableEditOnTemplate" },
        { EPropertyFlags::Transient,                      "Transient" },
        { EPropertyFlags::Config,                         "Config" },
        { EPropertyFlags::DisableEditOnInstance,          "DisableEditOnInstance" },
        { EPropertyFlags::EditConst,                      "EditConst" },
        { EPropertyFlags::GlobalConfig,                   "GlobalConfig" },
        { EPropertyFlags::InstancedReference,             "InstancedReference" },
        { EPropertyFlags::DuplicateTransient,             "DuplicateTransient" },
        { EPropertyFlags::SubobjectReference,             "SubobjectReference" },
        { EPropertyFlags::SaveGame,                       "SaveGame" },
        { EPropertyFlags::NoClear,                        "NoClear" },
        { EPropertyFlags::ReferenceParm,                  "ReferenceParm" },
        { EPropertyFlags::BlueprintAssignable,            "BlueprintAssignable" },
        { EPropertyFlags::Deprecated,                     "Deprecated" },
        { EPropertyFlags::IsPlainOldData,                 "IsPlainOldData" },
        { EPropertyFlags::RepSkip,                        "RepSkip" },
        { EPropertyFlags::RepNotify,                      "RepNotify" },
        { EPropertyFlags::Interp,                         "Interp" },
        { EPropertyFlags::NonTransactional,               "NonTransactional" },
        { EPropertyFlags::EditorOnly,                     "EditorOnly" },
        { EPropertyFlags::NoDestructor,                   "NoDestructor" },
        { EPropertyFlags::AutoWeak,                       "AutoWeak" },
        { EPropertyFlags::ContainsInstancedReference,     "ContainsInstancedReference" },
        { EPropertyFlags::AssetRegistrySearchable,        "AssetRegistrySearchable" },
        { EPropertyFlags::SimpleDisplay,                  "SimpleDisplay" },
        { EPropertyFlags::AdvancedDisplay,                "AdvancedDisplay" },
        { EPropertyFlags::Protected,                      "Protected" },
        { EPropertyFlags::BlueprintCallable,              "BlueprintCallable" },
        { EPropertyFlags::BlueprintAuthorityOnly,         "BlueprintAuthorityOnly" },
        { EPropertyFlags::TextExportTransient,            "TextExportTransient" },
        { EPropertyFlags::NonPIEDuplicateTransient,       "NonPIEDuplicateTransient" },
        { EPropertyFlags::ExposeOnSpawn,                  "ExposeOnSpawn" },
        { EPropertyFlags::PersistentInstance,             "PersistentInstance" },
        { EPropertyFlags::UObjectWrapper,                 "UObjectWrapper" },
        { EPropertyFlags::HasGetValueTypeHash,            "HasGetValueTypeHash" },
        { EPropertyFlags::NativeAccessSpecifierPublic,    "NativeAccessSpecifierPublic" },
        { EPropertyFlags::NativeAccessSpecifierProtected, "NativeAccessSpecifierProtected" },
        { EPropertyFlags::NativeAccessSpecifierPrivate,   "NativeAccessSpecifierPrivate" },
        { EPropertyFlags::SkipSerialization,              "SkipSerialization" },
    };
    std::string result;
    bool first = true;
    for (const auto& e : FlagEntries) {
        if (mask & static_cast<uint64_t>(e.Flag)) {
            if (!first) result += '|';
            result += e.Name;
            first = false;
        }
    }
    return result.empty() ? "Unknown" : result;
}

std::string DumpPropertyFlags(uint64_t flags)
{
    return DumpPropertyFlags(static_cast<SDK::EPropertyFlags>(flags));
}
