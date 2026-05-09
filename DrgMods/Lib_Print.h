#pragma once
// Lib_Print.h — PrintFieldValue, DumpItemProperties, BuildClassChain, Detail.

#include <string>
#include <vector>
#include "Lib_Forward.h"
#include "Lib_ObjectCast.h"
#include "Lib_PropertyAccess.h"
#include "Lib_Utils.h"

// =========================================================================
// Class chain helpers
// =========================================================================

inline std::vector<UStruct*> BuildClassChain(UClass* Class, UClass* OuterBase = nullptr)
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

inline void PrintClassHierarchy(const UClass* Class)
{
    for (const UStruct* c = Class; c; c = c->SuperStruct)
        spdlog::info("{}", c->GetName());
}

// =========================================================================
// Detail namespace
// =========================================================================

namespace Detail
{
    inline std::string Prefix(const std::vector<bool>& ancestry, bool isLast)
    {
        std::string r = "║   ";
        for (bool open : ancestry) r += open ? "│   " : "    ";
        r += isLast ? "└── " : "├── ";
        return r;
    }
    inline std::string ChildPrefix(const std::vector<bool>& ancestry, bool open)
    {
        std::string r = "║   ";
        for (bool a : ancestry) r += a ? "│   " : "    ";
        r += open ? "│   " : "    ";
        return r;
    }
    inline std::string ClassTag(const UClass* cls)
    {
        if (IsNativeClass(cls) && IsBlueprintClass(cls)) return "native+bp";
        if (IsNativeClass(cls))                          return "native";
        if (IsBlueprintClass(cls))                       return "blueprint";
        return "unknown";
    }
} // namespace Detail

// Forward declarations for recursion
inline void PrintFieldValue(uintptr_t Base, FField* Field, const std::vector<bool>& ancestry, bool isLast, const std::string& labelOverride = "");
inline void PrintFieldValue(UObject* Base, FField* Field, const std::vector<bool>& ancestry, bool isLast, const std::string& labelOverride = "");
inline std::string GetFieldValueAsString(uintptr_t Base, FField* Field);
// =========================================================================
// PrintFieldValue
// =========================================================================

inline void PrintFieldValue(uintptr_t Base, FField* Field,
    const std::vector<bool>& ancestry, bool isLast,
    const std::string& labelOverride)
{
    const std::string pre = Detail::Prefix(ancestry, isLast);
    FieldCast::Visit(Field, [&](auto* prop)
        {
            using T = std::remove_pointer_t<decltype(prop)>;
            const std::string fname = labelOverride.empty() ? prop->Name.ToString() : labelOverride;
            auto* fakeBase = reinterpret_cast<UObject*>(Base);

            if constexpr (std::is_same_v<T, FIntProperty>)    spdlog::info("{}{} = {}", pre, fname, *GetPropertyPtr<int32>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FInt8Property>)   spdlog::info("{}{} = {}", pre, fname, *GetPropertyPtr<int8>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FInt16Property>)  spdlog::info("{}{} = {}", pre, fname, *GetPropertyPtr<int16>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FInt64Property>)  spdlog::info("{}{} = {}", pre, fname, *GetPropertyPtr<int64>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FUInt16Property>) spdlog::info("{}{} = {}", pre, fname, *GetPropertyPtr<uint16>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FUInt32Property>) spdlog::info("{}{} = {}", pre, fname, *GetPropertyPtr<uint32>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FUInt64Property>) spdlog::info("{}{} = {}", pre, fname, *GetPropertyPtr<uint64>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FFloatProperty>)  spdlog::info("{}{} = {:.4f}", pre, fname, *GetPropertyPtr<float>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FDoubleProperty>) spdlog::info("{}{} = {:.6f}", pre, fname, *GetPropertyPtr<double>(Base, prop->Offset));
            else if constexpr (std::is_same_v<T, FByteProperty>)
            {
                const uint8 val = *GetPropertyPtr<uint8>(Base, prop->Offset);
                if (prop->Enum) spdlog::info("{}{} = {} ({})", pre, fname, val, prop->Enum->GetName());
                else            spdlog::info("{}{} = {}", pre, fname, val);
            }
            else if constexpr (std::is_same_v<T, FBoolProperty>)
                spdlog::info("{}{} = {}", pre, fname, ReadBool(fakeBase, prop));
            else if constexpr (std::is_same_v<T, FStrProperty>)
            {
                const auto& str = *GetPropertyPtr<UC::FString>(Base, prop->Offset);
                spdlog::info("{}{} = \"{}\"", pre, fname, str.IsValid() ? str.ToString() : "<empty>");
            }
            else if constexpr (std::is_same_v<T, FNameProperty>)
                spdlog::info("{}{} = {}", pre, fname, GetPropertyRef<FName>(fakeBase, prop->Offset).ToString());
            else if constexpr (std::is_same_v<T, FTextProperty>)
                spdlog::info("{}{} = [FText]", pre, fname);
            else if constexpr (std::is_same_v<T, FEnumProperty>)
            {
                int64 enumVal = 0;
                memcpy(&enumVal, GetPropertyPtr<void>(Base, prop->Offset), prop->UnderlayingProperty->ElementSize);
                spdlog::info("{}{} = {} ({})", pre, fname, enumVal, prop->Enum ? prop->Enum->GetName() : "?");
            }
            else if constexpr (std::is_same_v<T, FClassProperty>)
            {
                auto* cls = *GetPropertyPtr<UClass*>(Base, prop->Offset);
                spdlog::info("{}{} = {}", pre, fname, cls && IsValid(cls) ? cls->GetName() : "null");
            }
            else if constexpr (std::is_same_v<T, FObjectProperty> || std::is_same_v<T, FObjectPropertyBase>)
            {
                auto* obj = *GetPropertyPtr<UObject*>(Base, prop->Offset);
                spdlog::info("{}{} = {} ({})", pre, fname,
                    obj && IsValidRaw(obj) ? obj->GetName() : "null",
                    obj && IsValidRaw(obj) ? obj->Class->GetName() : "?");
            }
            else if constexpr (std::is_same_v<T, FStructProperty>)
            {
                spdlog::info("{}{} ({}):", pre, fname, prop->Struct->GetName());
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
                spdlog::info("{}{} [TArray {}/{}] <{}>", pre, fname, num, arr.Max(),
                    prop->InnerProperty->ClassPrivate->Name.ToString());
                if (arr.IsValid() && num > 0)
                {
                    const int32 es = prop->InnerProperty->ElementSize;
                    const uintptr_t db = reinterpret_cast<uintptr_t>(arr.GetDataPtr());
                    std::vector<bool> ca = ancestry; ca.push_back(!isLast);
                    for (int32 i = 0; i < maxShow; ++i)
                    {
                        const bool le = (i == maxShow - 1) && (num <= maxShow);
                        spdlog::info("{}[{}]", Detail::Prefix(ca, le), i);
                        std::vector<bool> ea = ca; ea.push_back(!le);
                        PrintFieldValue(db + i * es, prop->InnerProperty, ea, true);
                    }
                    if (num > maxShow) spdlog::info("{}... ({} more)", Detail::ChildPrefix(ancestry, !isLast), num - maxShow);
                }
            }
            else if constexpr (std::is_same_v<T, FSetProperty>)
            {
                const uintptr_t sb = Base + prop->Offset;
                const uintptr_t dp = *reinterpret_cast<const uintptr_t*>(sb);
                const int32     na = *reinterpret_cast<const int32*>(sb + 0x08);
                const uintptr_t bab = sb + 0x10;
                const int32* id = reinterpret_cast<const int32*>(bab);
                const int32* sd = *reinterpret_cast<int32* const*>(bab + 0x10);
                const uint32* bd = sd ? reinterpret_cast<const uint32*>(sd) : reinterpret_cast<const uint32*>(id);
                const int32 es = prop->ElementProperty->ElementSize + 8;
                int32 lc = 0;
                for (int32 i = 0; i < na; ++i) if (bd && (bd[i / 32] & (1u << (i % 32)))) ++lc;
                spdlog::info("{}{} [TSet {}/{}] <{}>", pre, fname, lc, na, prop->ElementProperty->ClassPrivate->Name.ToString());
                if (!dp || na <= 0) return;
                std::vector<bool> ca = ancestry; ca.push_back(!isLast);
                const int32 ms = (std::min)(lc, 8); int32 shown = 0;
                for (int32 i = 0; i < na && shown < ms; ++i)
                {
                    if (!bd || !(bd[i / 32] & (1u << (i % 32)))) continue;
                    const bool le = (shown == ms - 1) && (lc <= ms);
                    spdlog::info("{}[{}]", Detail::Prefix(ca, le), shown);
                    PrintFieldValue(dp + i * es, prop->ElementProperty, ca, true);
                    ++shown;
                }
                if (lc > ms) spdlog::info("{}... ({} more)", Detail::ChildPrefix(ancestry, !isLast), lc - ms);
            }
            else if constexpr (std::is_same_v<T, FMapProperty>)
            {
                const uintptr_t mb = Base + prop->Offset;
                const uintptr_t dp = *reinterpret_cast<const uintptr_t*>(mb);
                const int32     na = *reinterpret_cast<const int32*>(mb + 0x08);
                const uintptr_t bab = mb + 0x10;
                const int32* id = reinterpret_cast<const int32*>(bab);
                const int32* sd = *reinterpret_cast<int32* const*>(bab + 0x10);
                const uint32* bd = sd ? reinterpret_cast<const uint32*>(sd) : reinterpret_cast<const uint32*>(id);
                const int32 ps = (((prop->ValueProperty->Offset + prop->ValueProperty->ElementSize) + 7) & ~7) + 8;
                int32 lc = 0;
                for (int32 i = 0; i < na; ++i) if (bd && (bd[i / 32] & (1u << (i % 32)))) ++lc;
                spdlog::info("{}{} [TMap {}/{}] <{}, {}>", pre, fname, lc, na,
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
                    spdlog::info("{}[{}]", Detail::Prefix(ca, le), shown);
                    std::vector<bool> ea = ca; ea.push_back(!le);
                    PrintFieldValue(pb, prop->KeyProperty, ea, false, "key");
                    PrintFieldValue(pb, prop->ValueProperty, ea, true, "val");
                    ++shown;
                }
                if (lc > ms) spdlog::info("{}... ({} more)", Detail::ChildPrefix(ancestry, !isLast), lc - ms);
            }
            else if constexpr (std::is_base_of_v<FProperty, T>)
                spdlog::info("{}{} @ +{:#x} ({} bytes) [{}]", pre, fname, prop->Offset, prop->ElementSize,
                    prop->ClassPrivate ? prop->ClassPrivate->Name.ToString() : "?");
        });
}

inline void PrintFieldValue(UObject* Base, FField* Field,
    const std::vector<bool>& ancestry, bool isLast,
    const std::string& labelOverride)
{
    PrintFieldValue(reinterpret_cast<uintptr_t>(Base), Field, ancestry, isLast, labelOverride);
}

template <bool bSort = false>
inline void DumpItemProperties(UObject* Item, UClass* OuterBase = nullptr)
{
    if (!Item || !IsValid(Item)) return;

    const auto chain = BuildClassChain(Item->Class, OuterBase);
    spdlog::info("╔══ {} ({})", Item->GetName(), Item->Class->GetName());

    for (size_t i = 0; i < chain.size(); ++i)
    {
        UStruct* level = chain[i];
        UClass* asClass = static_cast<UClass*>(level);

        spdlog::info("╠═ [{}] {}", Detail::ClassTag(asClass), level->GetName());

        if (!level->ChildProperties) {
            spdlog::info("║   (no properties)");
            continue;
        }

        std::vector<FField*> fields;
        for (FField* field : FFieldRange(level->ChildProperties))
        {
            fields.push_back(field);
        }

        // Optional multi-criteria sort
        if constexpr (bSort)
        {
            std::sort(fields.begin(), fields.end(), [](FField* A, FField* B) {
                // 1. Get Type Names (e.g. "ByteProperty", "ObjectProperty")
                // Usually accessed via FField->Class->Name
                auto TypeA = A->ClassPrivate->Name.ToString();
                auto TypeB = B->ClassPrivate->Name.ToString();

                // If types are different, sort by type
                if (TypeA != TypeB)
                {
                    return TypeA < TypeB;
                }

                // 2. If types are the same, sort by the variable name
                return A->Name.ToString() < B->Name.ToString();
                });
        }

        const std::vector<bool> root{};
        for (size_t j = 0; j < fields.size(); ++j)
        {
            PrintFieldValue(reinterpret_cast<uintptr_t>(Item), fields[j], root, j == fields.size() - 1);
        }
    }
    spdlog::info("╚══════════════════════════");
}

inline void DumpItemPropertiesSorted(UObject* Item, UClass* OuterBase = nullptr)
{
    if (!Item || !IsValid(Item)) return;

    // Use your existing class chain builder
    auto chain = BuildClassChain(Item->Class, OuterBase);

    spdlog::info("╔══ {} ({})", Item->GetName(), Item->Class->GetName());

    for (auto * structlevel : chain)
    {
        auto level = ObjectCast::Cast<UClass>(structlevel);
        if (!level) continue;
        spdlog::info("╠═ [{}] {}", Detail::ClassTag(level), level->GetName());

        if (!level->ChildProperties) {
            spdlog::info("║   (no properties)");
            continue;
        }

        struct PropInfo {
            std::string Name;
            std::string Type;
        };
        std::vector<PropInfo> propList;

        // 1. Collect names and types
        for (FField* field : FFieldRange(level->ChildProperties))
        {
            auto* prop = FieldCast::Cast<FProperty>(field);
            if (!prop) continue;

            PropInfo info;
            info.Name = prop->Name.ToString();

            // Get the base type name (e.g., "IntProperty")
            info.Type = prop->ClassPrivate ? prop->ClassPrivate->Name.ToString() : "UnknownType";

            // Optional: Decorate type with specifics (like Struct name or Class name)
            if (auto* sp = FieldCast::Cast<FStructProperty>(prop))
                info.Type += std::format("<{}>", sp->Struct->GetName());
            else if (auto* op = FieldCast::Cast<FObjectPropertyBase>(prop))
                info.Type += std::format("<{}>", op->PropertyClass->GetName());
            else if (auto* ap = FieldCast::Cast<FArrayProperty>(prop))
                info.Type += std::format("<{}>", ap->InnerProperty->ClassPrivate->Name.ToString());

            propList.push_back(info);
        }

        // 2. Sort: Primary by Type, Secondary by Name
        std::sort(propList.begin(), propList.end(), [](const PropInfo& a, const PropInfo& b) {
            if (a.Type != b.Type) return a.Type < b.Type;
            return a.Name < b.Name;
            });

        // 3. Print
        for (const auto& p : propList)
        {
            spdlog::info("║   {:<35} : {}", p.Name, p.Type);
        }
    }
    spdlog::info("╚══════════════════════════");
}

inline std::string GetFieldValueAsString(uintptr_t Base, FField* Field)
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
            {
                float val = *GetPropertyPtr<float>(Base, prop->Offset);
                result = std::format("{:.6g}", val);
            }
            else if constexpr (std::is_same_v<T, FDoubleProperty>)
            {
                double val = *GetPropertyPtr<double>(Base, prop->Offset);
                result = std::format("{:.6g}", val);
            }
            else if constexpr (std::is_same_v<T, FByteProperty>)
            {
                uint8 val = *GetPropertyPtr<uint8>(Base, prop->Offset);
                result = std::to_string(val);
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
                result = std::to_string(enumVal);
            }
            else if constexpr (std::is_same_v<T, FClassProperty>)
            {
                auto* cls = *GetPropertyPtr<UClass*>(Base, prop->Offset);
                result = cls && IsValid(cls) ? cls->GetName() : "null";
            }
            else if constexpr (std::is_same_v<T, FObjectProperty> || std::is_same_v<T, FObjectPropertyBase>)
            {
                auto* obj = *GetPropertyPtr<UObject*>(Base, prop->Offset);
                result = obj && IsValidRaw(obj) ? obj->GetName() : "null";
            }
            else
                result = "<unsupported>";
        });
    return result;
}