// Lib_ObjectView.cpp — game-thread builder for the Objects-tab model (see Lib_ObjectView.h).
#include "Lib_ObjectView.h"
#include "Lib_Print.h"            // BuildClassChain, GetFieldValueAsString
#include "Lib_ObjectCast.h"       // IsValidRaw, ObjectCast::Cast
#include "Lib_PropertyAccess.h"   // GetPropertyPtr, GetTypeName, ReadBool
#include "Lib_PropertyInspector.h"// WriteProperty (path-resolved writes)

#include <algorithm>
#include <string>
#include <vector>

namespace ObjView
{
    using namespace SDK;

    namespace
    {
        constexpr int   kMaxArrShow  = 48;
        constexpr int   kMaxDepth     = 8;   // hard stop even if the expanded set is huge/deep

        // ── path-key mixing (must match what the UI toggles) ────────────────────
        inline uint64 Mix(uint64 h, uint64 v)
        {
            h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
        inline bool InExpanded(const Request& req, uint64 key)
        {
            return std::binary_search(req.expanded.begin(), req.expanded.end(), key);
        }

        // ── Vector3-shaped structs (editable as a DragFloat3) ────────────────────
        bool IsVector3Struct(const std::string& n)
        {
            return n == "Vector" || n == "Rotator"
                || n == "Vector_NetQuantize"    || n == "Vector_NetQuantize10"
                || n == "Vector_NetQuantize100" || n == "Vector_NetQuantizeNormal";
        }

        // Fill enum entry labels/values from a UEnum (prefix-stripped, _MAX dropped).
        void BuildEnumEntries(UEnum* e, std::vector<std::string>& names, std::vector<int64>& values)
        {
            if (!e) return;
            for (int32 i = 0; i < e->Names.Num(); ++i)
            {
                std::string s = e->Names[i].First.ToString();
                if (const auto pos = s.rfind("::"); pos != std::string::npos) s = s.substr(pos + 2);
                if (s == "_MAX" || s.ends_with("_MAX")) continue;
                names.push_back(std::move(s));
                values.push_back(e->Names[i].Second);
            }
        }

        std::string PrettyType(FProperty* p) { return p ? GetTypeName(p) : "?"; }

        // Map a property to the EditKind the shared widget builder understands (type only — no
        // value read). Used for function-arg widgets; object/non-vector-struct fall to ReadOnly
        // (the arg builder renders those as a text token WriteParam parses, incl. object-by-name).
        EditKind ClassifyParamEdit(FProperty* p, std::vector<std::string>& enumNames,
                                   std::vector<int64>& enumValues, int32& enumSize)
        {
            if (FieldCast::IsA<FBoolProperty>(p))   return EditKind::Bool;
            if (FieldCast::IsA<FIntProperty>(p))    return EditKind::Int;
            if (FieldCast::IsA<FFloatProperty>(p))  return EditKind::Float;
            if (FieldCast::IsA<FDoubleProperty>(p)) return EditKind::Double;
            if (FieldCast::IsA<FNameProperty>(p))   return EditKind::Name;
            if (FieldCast::IsA<FStrProperty>(p))    return EditKind::Str;
            if (FieldCast::IsA<FTextProperty>(p))   return EditKind::Text;
            if (auto* ep = FieldCast::Cast<FEnumProperty>(p))
            { enumSize = ep->UnderlayingProperty ? ep->UnderlayingProperty->ElementSize : 1; BuildEnumEntries(ep->Enum, enumNames, enumValues); return EditKind::Enum; }
            if (auto* bp = FieldCast::Cast<FByteProperty>(p); bp && bp->Enum)
            { enumSize = 1; BuildEnumEntries(bp->Enum, enumNames, enumValues); return EditKind::Enum; }
            if (auto* sp = FieldCast::Cast<FStructProperty>(p))
                return (sp->Struct && IsVector3Struct(sp->Struct->GetName())) ? EditKind::VectorLike : EditKind::ReadOnly;
            if (FieldCast::IsA<FObjectProperty>(p) || FieldCast::IsA<FClassProperty>(p)) return EditKind::Object;
            return EditKind::ReadOnly;
        }

        // ── one property → PropNode ──────────────────────────────────────────────
        // `base` is the absolute address of the container holding `prop`; `pathToBase` are the
        // hops that reach `base` from the root; `parentKey` seeds this node's stable key.
        PropNode BuildNode(uintptr_t base, FProperty* prop, const std::string& name,
                           const PropPath& pathToBase, uint64 parentKey, int depth,
                           const Request& req);

        void BuildStructFields(uintptr_t base, UStruct* ustruct, const PropPath& pathToBase,
                               uint64 levelKey, int depth, const Request& req,
                               std::vector<PropNode>& out)
        {
            if (!ustruct || depth > kMaxDepth || !IsValidRaw(ustruct)) return;
            if (!ustruct->ChildProperties) return;
            for (FField* f : FFieldRange(ustruct->ChildProperties))
            {
                auto* p = FieldCast::Cast<FProperty>(f);
                if (!p) continue;
                out.push_back(BuildNode(base, p, p->Name.ToString(), pathToBase, levelKey, depth, req));
            }
        }

        PropNode BuildNode(uintptr_t base, FProperty* prop, const std::string& name,
                           const PropPath& pathToBase, uint64 parentKey, int depth,
                           const Request& req)
        {
            const uint64 key = Mix(parentKey, (uint64)prop->Offset + 1);
            PropNode n;
            n.name     = name;
            n.depth    = depth;
            n.key      = key;
            n.typeName = GetTypeName(prop);   // shown next to the name; struct/array override below
            // Leaf write addressing: hops reach `base`, leaf is this prop (offset applied by the
            // write resolver). Struct/array children extend the hops below.
            n.path           = pathToBase;
            n.path.leafProp  = reinterpret_cast<uint64>(prop);

            FField* field = prop;
            auto valStr = [&]{ return GetFieldValueAsString(base, field); };

            if (auto* bp = FieldCast::Cast<FBoolProperty>(field))
            {
                n.edit = EditKind::Bool;
                n.boolVal = ReadBool(reinterpret_cast<UObject*>(base), bp);
                n.valueStr = n.boolVal ? "true" : "false";
            }
            else if (FieldCast::IsA<FIntProperty>(field))
            {
                n.edit = EditKind::Int;
                n.valueStr = std::to_string(*GetPropertyPtr<int32>(base, prop->Offset));
            }
            else if (FieldCast::IsA<FFloatProperty>(field))
            {
                n.edit = EditKind::Float;
                n.valueStr = valStr();
            }
            else if (FieldCast::IsA<FDoubleProperty>(field))
            {
                n.edit = EditKind::Double;
                n.valueStr = valStr();
            }
            else if (FieldCast::IsA<FNameProperty>(field))
            {
                n.edit = EditKind::Name;
                n.valueStr = GetPropertyPtr<FName>(base, prop->Offset)->ToString();
            }
            else if (FieldCast::IsA<FStrProperty>(field))
            {
                n.edit = EditKind::Str;
                auto* s = GetPropertyPtr<FString>(base, prop->Offset);
                n.valueStr = s->IsValid() ? s->ToString() : std::string{};
            }
            else if (FieldCast::IsA<FTextProperty>(field))
            {
                n.edit = EditKind::Text;
                auto* t = GetPropertyPtr<FText>(base, prop->Offset);
                n.valueStr = t->TextData ? t->TextData->TextSource.ToString() : std::string{};
            }
            else if (auto* ep = FieldCast::Cast<FEnumProperty>(field))
            {
                n.edit = EditKind::Enum;
                n.enumSize = ep->UnderlayingProperty ? ep->UnderlayingProperty->ElementSize : 1;
                BuildEnumEntries(ep->Enum, n.enumNames, n.enumValues);
                uint8* slot = GetPropertyPtr<uint8>(base, prop->Offset);
                switch (n.enumSize) {
                case 2: n.enumValue = *reinterpret_cast<uint16*>(slot); break;
                case 4: n.enumValue = *reinterpret_cast<uint32*>(slot); break;
                case 8: n.enumValue = *reinterpret_cast<int64*> (slot); break;
                default: n.enumValue = *slot; break; }
                n.valueStr = valStr();
            }
            else if (auto* bep = FieldCast::Cast<FByteProperty>(field); bep && bep->Enum)
            {
                n.edit = EditKind::Enum;
                n.enumSize = 1;
                BuildEnumEntries(bep->Enum, n.enumNames, n.enumValues);
                n.enumValue = *GetPropertyPtr<uint8>(base, prop->Offset);
                n.valueStr = valStr();
            }
            else if (auto* sp = FieldCast::Cast<FStructProperty>(field))
            {
                const std::string sname = sp->Struct ? sp->Struct->GetName() : "?";
                n.typeName = sname;
                if (IsVector3Struct(sname))
                {
                    n.valueStr = valStr();
                    if (sname == "Rotator") {
                        auto* r = GetPropertyPtr<SDK::FRotator>(base, prop->Offset);
                        n.edit = EditKind::VectorLike;
                        n.vec3[0]=(float)r->Pitch; n.vec3[1]=(float)r->Yaw; n.vec3[2]=(float)r->Roll;
                    } else {
                        auto* v = GetPropertyPtr<SDK::FVector>(base, prop->Offset);
                        n.edit = EditKind::VectorLike;
                        n.vec3[0]=(float)v->X; n.vec3[1]=(float)v->Y; n.vec3[2]=(float)v->Z;
                    }
                }
                else
                {
                    n.edit = EditKind::ReadOnly;
                    n.hasChildren = sp->Struct && sp->Struct->ChildProperties;
                    n.valueStr = valStr();
                    if (n.hasChildren && depth < kMaxDepth && InExpanded(req, key))
                    {
                        n.expanded = true;
                        PropPath childPath = pathToBase;
                        childPath.hops.push_back({ PropHop::Kind::Struct, prop->Offset, 0, 0 });
                        BuildStructFields(base + prop->Offset, sp->Struct, childPath, key,
                                          depth + 1, req, n.children);
                    }
                }
            }
            else if (auto* ap = FieldCast::Cast<FArrayProperty>(field))
            {
                n.container = Container::Array;
                n.typeName  = PrettyType(ap->InnerProperty);
                const auto& arr = *GetPropertyPtr<UC::TArray<uint8>>(base, prop->Offset);
                n.containerNum = arr.Num();
                n.hasChildren  = n.containerNum > 0 && ap->InnerProperty;
                n.valueStr = valStr();
                if (n.hasChildren && depth < kMaxDepth && InExpanded(req, key))
                {
                    n.expanded = true;
                    const int32 es = ap->InnerProperty->ElementSize;
                    const int32 show = std::min(n.containerNum, kMaxArrShow);
                    const uintptr_t db = reinterpret_cast<uintptr_t>(arr.GetDataPtr());
                    for (int32 i = 0; i < show && db; ++i)
                    {
                        PropPath elemPath = pathToBase;
                        elemPath.hops.push_back({ PropHop::Kind::ArrayElem, prop->Offset, i, es });
                        PropNode child = BuildNode(db + (uintptr_t)i * es, ap->InnerProperty,
                                                   "[" + std::to_string(i) + "]", elemPath,
                                                   Mix(key, 0xE1E), depth + 1, req);
                        child.path.leafIsArrayElem = true;
                        n.children.push_back(std::move(child));
                    }
                }
            }
            else if (FieldCast::IsA<FObjectProperty>(field) || FieldCast::IsA<FClassProperty>(field))
            {
                n.edit = EditKind::Object;
                auto* ptr = *GetPropertyPtr<UObject*>(base, prop->Offset);
                const bool ok = ptr && IsValidRaw(ptr);
                n.objectAddr = ok ? (uint64)ptr : 0;
                n.valueStr = ok ? (std::string(ptr->Class ? ptr->Class->GetName() : "?")
                                   + "'" + ptr->GetName() + "'")
                                : "nullptr";
            }
            else if (FieldCast::IsA<FMapProperty>(field))
            {
                n.container = Container::Map; n.edit = EditKind::ReadOnly; n.valueStr = valStr();
            }
            else if (FieldCast::IsA<FSetProperty>(field))
            {
                n.container = Container::Set; n.edit = EditKind::ReadOnly; n.valueStr = valStr();
            }
            else
            {
                n.edit = EditKind::ReadOnly;
                n.valueStr = valStr();
            }
            return n;
        }

        FuncView BuildFunc(UFunction* func)
        {
            FuncView fv;
            fv.name   = func->GetName();
            fv.funcId = reinterpret_cast<uint64>(func);
            const auto fl = static_cast<EFunctionFlags>(func->FunctionFlags);
            if (fl & EFunctionFlags::BlueprintCallable) fv.suffix += " [BP]";
            if (fl & EFunctionFlags::BlueprintEvent)    fv.suffix += " [Ev]";
            if (fl & EFunctionFlags::BlueprintPure)     fv.suffix += " [Pure]";
            if (fl & EFunctionFlags::Exec)              fv.suffix += " [Exec]";
            if (fl & EFunctionFlags::Native)            fv.suffix += " [Native]";
            if (fl & EFunctionFlags::Static)            fv.suffix += " [Static]";
            if (fl & EFunctionFlags::Net)               fv.suffix += " [Net]";
            for (FField* f : FFieldRange(func->ChildProperties))
            {
                auto* p = FieldCast::Cast<FProperty>(f);
                if (!p) continue;
                const auto pf = static_cast<EPropertyFlags>(p->PropertyFlags);
                if (!(pf & EPropertyFlags::Parm)) continue;
                // Param direction (CPF_OutParm is set on every by-ref param, so it alone doesn't
                // mean "output"). 0=in, 1=pure-out (write-only), 2=return, 3=in-out (UPARAM(ref):
                // non-const reference the function reads AND writes). Must match
                // InvokeFunctionGameThread's needsInput/readBack split below.
                const bool isRet  = !!(pf & EPropertyFlags::ReturnParm);
                const bool out    = (pf & EPropertyFlags::OutParm) && !(pf & EPropertyFlags::ConstParm);
                const bool byRef  = !!(pf & EPropertyFlags::ReferenceParm);
                int dir = 0;                       // plain input or const-ref input
                if (isRet)      dir = 2;
                else if (out)   dir = byRef ? 3 : 1;   // in-out vs pure-out
                ParamView pv;
                pv.name = p->Name.ToString();
                pv.type = GetTypeName(p);
                pv.dir  = dir;
                pv.edit = ClassifyParamEdit(p, pv.enumNames, pv.enumValues, pv.enumSize);
                fv.params.push_back(std::move(pv));
            }
            fv.invokable =
                (fl & EFunctionFlags::BlueprintCallable) || (fl & EFunctionFlags::BlueprintEvent) ||
                (fl & EFunctionFlags::BlueprintPure)     || (fl & EFunctionFlags::Exec);
            return fv;
        }
    } // namespace

    uint64 ChildKey(uint64 parentKey, uint64 discriminator) { return Mix(parentKey, discriminator); }

    bool WritePath(const PropPath& path, const std::string& value)
    {
        auto* root = reinterpret_cast<UObject*>(path.rootAddr);
        if (!root) return false;
        // Validate by index against GObjects before dereferencing (the root may be freed — same
        // wild-pointer hazard as BuildObjectView; IsValidRaw alone would deref a dead pointer).
        if (path.rootIndex < 0 || UObject::GObjects->GetByIndex(path.rootIndex) != root) return false;
        if (!IsValidRaw(root) || root->Index != path.rootIndex) return false;
        auto* leaf = reinterpret_cast<FProperty*>(path.leafProp);
        if (!leaf) return false;

        // FScriptArray runtime layout, re-read live at each ArrayElem hop (realloc-safe).
        struct FScriptArray { void* Data; int32 Num; int32 Max; };

        uintptr_t base = static_cast<uintptr_t>(path.rootAddr);
        for (const auto& hop : path.hops)
        {
            if (hop.kind == PropHop::Kind::Struct)
                base += hop.offset;
            else // ArrayElem
            {
                auto* a = reinterpret_cast<FScriptArray*>(base + hop.offset);
                if (!a->Data || hop.index < 0 || hop.index >= a->Num) return false;
                base = reinterpret_cast<uintptr_t>(a->Data) + (uintptr_t)hop.index * hop.elemSize;
            }
        }
        // WriteProperty applies leaf->Offset to `base` (0 for array-element inner props).
        PropertyInspector::WriteProperty(root, leaf, base, leaf, value, leaf->Name.ToString(), false, 0);
        return true;
    }

    ObjectView BuildObjectView(SDK::UObject* obj, const Request& req)
    {
        ObjectView v;
        if (!obj) return v;
        // The focus pointer may be FREED (e.g. the object was destroyed while the overlay was
        // hidden, so no rebuild caught it; on reopen we build the stale addr). IsValidRaw only
        // checks GC flags — it still dereferences the pointer, so a wild ptr crashes. When we have
        // a recorded index (normal selection), confirm the GObjects slot still maps to THIS exact
        // pointer via the bounds-checked array lookup BEFORE any deref.
        if (req.focusIndex >= 0 && UObject::GObjects->GetByIndex(req.focusIndex) != obj) return v;
        if (!IsValidRaw(obj) || !obj->Class || !IsValidRaw(obj->Class)) return v;
        if (req.focusIndex >= 0 && obj->Index != req.focusIndex)        return v;

        v.addr      = (uint64)obj;
        v.index     = obj->Index;
        v.valid     = true;
        v.objName   = obj->GetName();
        v.className = obj->Class->GetName();
        v.classAddr = (uint64)obj->Class;
        if (obj->Outer && IsValidRaw(obj->Outer))
        {
            v.outerName = obj->Outer->GetName();
            v.outerAddr = (uint64)obj->Outer;
        }

        const uintptr_t objBase = reinterpret_cast<uintptr_t>(obj);
        const auto chain = BuildClassChain(obj->Class, UObject::StaticClass());
        for (size_t lvl = 0; lvl < chain.size(); ++lvl)
        {
            UStruct* level = chain[lvl];
            if (!level || !IsValidRaw(level)) continue;

            ClassLevelView clv;
            clv.className = level->GetName();
            clv.level     = (int)lvl;
            const uint64 levelKey = ChildKey(0x100ULL + lvl, 0);

            PropPath rootPath;
            rootPath.rootAddr  = v.addr;
            rootPath.rootIndex = v.index;
            BuildStructFields(objBase, level, rootPath, levelKey, 0, req, clv.props);

            if (level->Children)
                for (UField* child : TLinkedListRange<UField>(level->Children))
                    if (auto* fn = ObjectCast::Cast<UFunction>(child))
                        clv.funcs.push_back(BuildFunc(fn));

            v.chain.push_back(std::move(clv));
        }
        return v;
    }
}
