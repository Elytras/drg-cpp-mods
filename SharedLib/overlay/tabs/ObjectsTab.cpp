// ObjectsTab.cpp — unified UObject browser.
//
// One table over all GObjects (ObjectList snapshot), enriched with actor-specific
// fields (replicated, owner, instigator, outerAddr, classChain) for rows that also
// appear in the ActorList snapshot, matched by pointer address. Non-actor objects
// show "-" for those columns and are excluded by actor-specific filters only when
// a non-empty token is typed (empty token list = pass all).
//
// Columns: class | name | net | outer | owner | instigator | addr (hidden by default).
//
// Click interactions:
//   class button  — jump to the UClass object (disables skip_internals so it's visible)
//   outer button  — jump to the outer object (works for both actors and plain objects)
//   owner button  — jump (actors only)
//   instigator    — jump (actors only)
//
// Filters: multi-token text (space/comma-separated, IStartsWith per token); class has
// a mode selector (Contains / Is / Is+Sub / Not / Not+Sub); net has a mode selector;
// type checkboxes (Internals / CDOs / BP / C++) control visibility by object kind.
//
// Property tree: BuildClassChain with UObject as the stop — shows the full ancestry
// from the selected class all the way down to UObject (not just the first C++ class).
//
// Split divider is draggable (ImGuiTableFlags_Resizable on the outer split table).
//
// Layer: game (Layer 3). Registered via detail::RegisterObjectsTab().

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../OverlayTabs.h"
#include "../Lib_OverlayUI.h"
#include "../../core/StringLib.h"
#include "../../game/Lib_ObjectCast.h"
#include "../../game/Lib_PropertyAccess.h"
#include "../../game/Lib_PropertyInspector.h"
#include "../../game/Lib_Print.h"
#include "../../hooks/Lib_GameHooks.h"

#include <imgui.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace OverlayConsole
{
namespace
{
    using detail::NowMs;
    using StringLib::IEquals;
    using StringLib::IContains;
    using StringLib::IStartsWith;
    using namespace SDK;

    // ── Multi-query helpers ──────────────────────────────────────────────────────

    static std::vector<std::string> ParseTokens(std::string_view input)
    {
        std::vector<std::string> out;
        size_t s = 0;
        while (s < input.size())
        {
            size_t e = input.find_first_of(" ,", s);
            if (e == std::string_view::npos) e = input.size();
            if (e > s) out.emplace_back(input.substr(s, e - s));
            s = e + 1;
        }
        return out;
    }

    // Pass if tokens is empty, or text case-insensitively starts with any token.
    static bool IStartsWithAny(std::string_view text, const std::vector<std::string>& tokens)
    {
        if (tokens.empty()) return true;
        for (const auto& t : tokens)
            if (IStartsWith(text, t)) return true;
        return false;
    }

    // ── Property tree ────────────────────────────────────────────────────────────

    constexpr int kMaxTreeDepth = 5;
    constexpr int kMaxArrShow   = 48;

    // Color by struct-nesting depth (0 = direct field of a class).
    static ImVec4 FieldDepthColor(int depth)
    {
        static constexpr ImVec4 kPalette[] = {
            { 0.88f, 0.88f, 0.88f, 1.f },   // 0  near-white
            { 0.45f, 0.80f, 1.00f, 1.f },   // 1  sky blue
            { 0.40f, 1.00f, 0.55f, 1.f },   // 2  mint
            { 1.00f, 0.85f, 0.28f, 1.f },   // 3  gold
            { 1.00f, 0.58f, 0.28f, 1.f },   // 4  orange
            { 0.78f, 0.48f, 1.00f, 1.f },   // 5  lavender
        };
        return kPalette[depth % (int)std::size(kPalette)];
    }

    // Color by class-inheritance level (0 = most derived class).
    static ImVec4 ClassLevelColor(int lvl)
    {
        static constexpr ImVec4 kPalette[] = {
            { 1.00f, 0.80f, 0.20f, 1.f },   // 0  gold   (leaf class)
            { 0.20f, 0.80f, 1.00f, 1.f },   // 1  cyan
            { 0.40f, 1.00f, 0.40f, 1.f },   // 2  green
            { 1.00f, 0.52f, 0.32f, 1.f },   // 3  coral
            { 0.72f, 0.44f, 1.00f, 1.f },   // 4  violet
            { 1.00f, 0.44f, 0.65f, 1.f },   // 5  rose
        };
        return kPalette[lvl % (int)std::size(kPalette)];
    }

    // ── Struct + function descriptor cache ──────────────────────────────────────
    // Built once per UStruct* (on first draw). Eliminates per-frame:
    //   · FField* chain walk      · FName::ToString() heap alloc
    //   · FieldCast::Cast() type dispatch    · GetName() on inner types

    enum class WKind : uint8 { Generic, Bool, Int, Float, Double, Name, Object, Struct, Array, MapOrSet };

    // Set by Object-field "→" buttons / class-outer buttons; consumed by the tab each frame.
    static uint64_t s_treeJumpAddr      = 0;
    static uint64_t s_treeJumpClassAddr = 0;

    // ── Inline field editing ──────────────────────────────────────────────────────
    // Root object of the tree currently being drawn — threaded via file statics (like
    // the jump addresses) so DrawFieldFromDesc can validate/log a write without an extra
    // param on every node. Set at the top of DrawObjectTree, valid for that draw only.
    static UObject* s_rootObj = nullptr;
    static int32    s_rootIdx = -1;

    // Single active text edit at a time, keyed by the field's absolute address. Avoids an
    // unbounded per-field std::string buffer: while a field is focused we keep its in-progress
    // text here; every other field mirrors its live value each frame (a fresh local copy).
    static uintptr_t s_editKey = 0;
    static std::string s_editText;

    // Editable single-line text widget. `live` is the current value (shown when not focused).
    // Returns true with the committed text in `out` when the user presses Enter.
    static bool EditTextField(const char* id, uintptr_t key, const std::string& live, std::string& out)
    {
        const bool active = (s_editKey == key);
        std::string buf = active ? s_editText : live;   // per-frame copy ImGui edits into
        UI::SetNextItemWidth(220.f);
        const bool entered = UI::InputTextString(id, buf, ImGuiInputTextFlags_EnterReturnsTrue);
        if (UI::IsItemActivated()) { s_editKey = key; s_editText = live; }
        if (s_editKey == key) s_editText = buf;          // capture this frame's keystrokes
        if (entered && active) { out = buf; s_editKey = 0; return true; }
        // Focus lost without committing → discard the pending edit.
        if (active && !entered && UI::IsItemDeactivated()) s_editKey = 0;
        return false;
    }

    // Queue a string→property write onto the game thread (name table / allocator are
    // game-thread-confined). Re-validates the root object by index before poking.
    static void EnqueuePropWrite(uintptr_t writeBase, FProperty* prop,
                                 const std::string& name, std::string value)
    {
        UObject*    root = s_rootObj;
        const int32 idx  = s_rootIdx;
        EnqueueOnce([root, idx, writeBase, prop, name, value]()
        {
            if (!IsValidRaw(root) || root->Index != idx) return;
            PropertyInspector::WriteProperty(root, prop, writeBase, prop, value, name, false, 0);
        });
    }

    struct FieldDesc {
        FField*        field;
        FProperty*     prop;
        std::string    name;        // avoids FName::ToString() per frame
        std::string    typeName;    // Struct: inner struct name; Array: elem type name
        int32          offset   = 0;
        WKind          kind     = WKind::Generic;
        FBoolProperty* boolProp    = nullptr;   // WKind::Bool
        UStruct*       innerStruct = nullptr;   // WKind::Struct
        FProperty*     innerProp   = nullptr;   // WKind::Array
        int32          elemSize    = 0;         // WKind::Array
    };

    struct FuncDesc {
        UFunction*   func;
        std::string  name;
        std::string  suffix;
        bool         hasInputParams = false;
        bool         callable       = false;
        int32        parmsSize      = 0;
        struct Param { std::string name, type; const char* dir; };
        std::vector<Param> params;
    };

    using StructDesc = std::vector<FieldDesc>;
    using FuncList   = std::vector<FuncDesc>;

    // Cache entries carry Index so a reused address (GC + new alloc at same ptr)
    // is detected and forces a rebuild rather than returning stale FField* pointers.
    struct StructDescEntry { int32 internalIndex = -1; StructDesc desc; };
    struct FuncListEntry   { int32 internalIndex = -1; FuncList   funcs; };

    static std::unordered_map<UStruct*, StructDescEntry> s_descCache;
    static std::unordered_map<UStruct*, FuncListEntry>   s_funcCache;

    // Map an FProperty to a human-readable type string for container element labels.
    static std::string PrettifyPropTypeName(FProperty* prop)
    {
        if (!prop) return "?";
        if (FieldCast::IsA<FBoolProperty>(prop))   return "bool";
        if (FieldCast::IsA<FIntProperty>(prop))    return "int32";
        if (FieldCast::IsA<FFloatProperty>(prop))  return "float";
        if (FieldCast::IsA<FDoubleProperty>(prop)) return "double";
        if (FieldCast::IsA<FNameProperty>(prop))   return "FName";
        if (auto* sp = FieldCast::Cast<FStructProperty>(prop))
            return sp->Struct ? sp->Struct->GetName() : "struct";
        if (FieldCast::IsA<FObjectProperty>(prop) || FieldCast::IsA<FClassProperty>(prop))
            return "UObject*";
        if (FieldCast::IsA<FArrayProperty>(prop))  return "TArray";
        if (FieldCast::IsA<FMapProperty>(prop))    return "TMap";
        if (FieldCast::IsA<FSetProperty>(prop))    return "TSet";
        // Fallback: strip "Property" suffix from FField class name
        if (prop->ClassPrivate)
        {
            std::string n = prop->ClassPrivate->Name.ToString();
            if (n.size() > 8 && n.ends_with("Property"))
                n.resize(n.size() - 8);
            return n;
        }
        return "?";
    }

    static const StructDesc& GetOrBuildStructDesc(UStruct* ustruct)
    {
        static const StructDesc s_empty;
        if (!ustruct) return s_empty;

        auto it = s_descCache.find(ustruct);
        if (it != s_descCache.end())
        {
            // Validate the cached entry is still for the same object instance.
            // IsValidRaw reads Index from the ptr; if that succeeds and
            // the index matches what we recorded, the struct is unchanged.
            if (IsValidRaw(ustruct) && ustruct->Index == it->second.internalIndex)
                return it->second.desc;
            s_descCache.erase(it);
        }

        if (!IsValidRaw(ustruct)) return s_empty;

        StructDescEntry entry;
        entry.internalIndex = ustruct->Index;
        if (ustruct->ChildProperties)
        {
            for (auto* f : FFieldRange(ustruct->ChildProperties))
            {
                auto* p = FieldCast::Cast<FProperty>(f);
                if (!p) continue;
                FieldDesc fd;
                fd.field  = f;
                fd.prop   = p;
                fd.name   = p->Name.ToString();
                fd.offset = p->Offset;
                if (auto* sp = FieldCast::Cast<FStructProperty>(f))
                {
                    fd.kind        = WKind::Struct;
                    fd.innerStruct = sp->Struct;
                    fd.typeName    = sp->Struct ? sp->Struct->GetName() : "?";
                }
                else if (auto* ap = FieldCast::Cast<FArrayProperty>(f))
                {
                    fd.kind      = WKind::Array;
                    fd.innerProp = ap->InnerProperty;
                    fd.elemSize  = ap->InnerProperty ? ap->InnerProperty->ElementSize : 0;
                    fd.typeName  = PrettifyPropTypeName(ap->InnerProperty);
                }
                else if (auto* bp = FieldCast::Cast<FBoolProperty>(f))
                {
                    fd.kind     = WKind::Bool;
                    fd.boolProp = bp;
                }
                else if (FieldCast::IsA<FIntProperty>(f))
                    fd.kind = WKind::Int;
                else if (FieldCast::IsA<FFloatProperty>(f))
                    fd.kind = WKind::Float;
                else if (FieldCast::IsA<FDoubleProperty>(f))
                    fd.kind = WKind::Double;
                else if (FieldCast::IsA<FNameProperty>(f))
                    fd.kind = WKind::Name;
                else if (FieldCast::IsA<FObjectProperty>(f) || FieldCast::IsA<FClassProperty>(f))
                    fd.kind = WKind::Object;
                else if (FieldCast::IsA<FMapProperty>(f) || FieldCast::IsA<FSetProperty>(f))
                    fd.kind = WKind::MapOrSet;
                entry.desc.push_back(std::move(fd));
            }
        }
        return s_descCache.emplace(ustruct, std::move(entry)).first->second.desc;
    }

    static const FuncList& GetOrBuildFuncList(UStruct* ustruct)
    {
        static const FuncList s_empty;
        if (!ustruct) return s_empty;

        auto it = s_funcCache.find(ustruct);
        if (it != s_funcCache.end())
        {
            if (IsValidRaw(ustruct) && ustruct->Index == it->second.internalIndex)
                return it->second.funcs;
            s_funcCache.erase(it);
        }

        if (!IsValidRaw(ustruct)) return s_empty;

        FuncListEntry entry;
        entry.internalIndex = ustruct->Index;
        if (ustruct->Children)
        {
            for (auto* child : TLinkedListRange<UField>(ustruct->Children))
            {
                auto* func = ObjectCast::Cast<UFunction>(child);
                if (!func) continue;
                const auto fflags = static_cast<EFunctionFlags>(func->FunctionFlags);
                FuncDesc fd;
                fd.func      = func;
                fd.name      = func->GetName();
                fd.parmsSize = func->Size;
                if (fflags & EFunctionFlags::BlueprintCallable) fd.suffix += " [BP]";
                if (fflags & EFunctionFlags::BlueprintEvent)    fd.suffix += " [Ev]";
                if (fflags & EFunctionFlags::BlueprintPure)     fd.suffix += " [Pure]";
                if (fflags & EFunctionFlags::Exec)              fd.suffix += " [Exec]";
                if (fflags & EFunctionFlags::Native)            fd.suffix += " [Native]";
                if (fflags & EFunctionFlags::Static)            fd.suffix += " [Static]";
                if (fflags & EFunctionFlags::Net)               fd.suffix += " [Net]";
                for (auto* f : FFieldRange(func->ChildProperties))
                {
                    auto* p = FieldCast::Cast<FProperty>(f);
                    if (!p) continue;
                    const auto pflags = static_cast<EPropertyFlags>(p->PropertyFlags);
                    if (!(pflags & EPropertyFlags::Parm)) continue;
                    const bool isReturn = !!(pflags & EPropertyFlags::ReturnParm);
                    const bool isOut    = !isReturn && !!(pflags & EPropertyFlags::OutParm);
                    if (!isReturn && !isOut) fd.hasInputParams = true;
                    fd.params.push_back({p->Name.ToString(), GetTypeName(p),
                        isReturn ? "[ret]" : isOut ? "[out]" : " [in]"});
                }
                fd.callable = !fd.hasInputParams && (
                    (fflags & EFunctionFlags::BlueprintCallable) ||
                    (fflags & EFunctionFlags::BlueprintEvent)    ||
                    (fflags & EFunctionFlags::BlueprintPure)     ||
                    (fflags & EFunctionFlags::Exec));
                entry.funcs.push_back(std::move(fd));
            }
        }
        return s_funcCache.emplace(ustruct, std::move(entry)).first->second.funcs;
    }

    // ─────────────────────────────────────────────────────────────────────────────

    static void DrawFieldNode(uintptr_t base, FField* field, const char* label, int depth);
    static void DrawStructCached(uintptr_t base, UStruct* ustruct, int depth);

    static void DrawFieldFromDesc(uintptr_t base, const FieldDesc& fd, int depth)
    {
        const ImVec4 col = FieldDepthColor(depth);
        switch (fd.kind)
        {
        case WKind::Struct:
            {
                if (depth >= kMaxTreeDepth)
                {
                    UI::PushStyleColor(ImGuiCol_Text, col);
                    UI::Text("%s = %s", fd.name.c_str(),
                        GetFieldValueAsString(base, fd.field).c_str());
                    UI::PopStyleColor();
                    return;
                }
                char head[512];
                snprintf(head, sizeof(head), "%s (%s) = %s",
                    fd.name.c_str(), fd.typeName.c_str(),
                    GetFieldValueAsString(base, fd.field).c_str());
                UI::PushStyleColor(ImGuiCol_Text, col);
                const bool open = UI::TreeNodeEx(fd.field, ImGuiTreeNodeFlags_None, "%s", head);
                UI::PopStyleColor();
                if (open)
                {
                    DrawStructCached(base + fd.offset, fd.innerStruct, depth + 1);
                    UI::TreePop();
                }
                break;
            }
        case WKind::Array:
            {
                const auto& arr = *GetPropertyPtr<UC::TArray<uint8>>(base, fd.offset);
                const int32 num = arr.Num();
                char head[512];
                snprintf(head, sizeof(head), "%s [%d] <%s>",
                    fd.name.c_str(), num, fd.typeName.c_str());
                UI::PushStyleColor(ImGuiCol_Text, col);
                const bool open = UI::TreeNodeEx(fd.field, ImGuiTreeNodeFlags_None, "%s", head);
                UI::PopStyleColor();
                if (open)
                {
                    if (arr.IsValid() && num > 0 && fd.innerProp && depth < kMaxTreeDepth)
                    {
                        const int32     show = std::min(num, kMaxArrShow);
                        const uintptr_t db   = reinterpret_cast<uintptr_t>(arr.GetDataPtr());
                        for (int32 i = 0; i < show; ++i)
                        {
                            char lbl[32];
                            snprintf(lbl, sizeof(lbl), "[%d]", i);
                            UI::PushID(i);
                            DrawFieldNode(db + (uintptr_t)i * fd.elemSize,
                                fd.innerProp, lbl, depth + 1);
                            UI::PopID();
                        }
                        if (num > show) UI::TextDisabled("  ... %d more", num - show);
                    }
                    UI::TreePop();
                }
                break;
            }
        case WKind::Bool:
            {
                bool val = ReadBool(reinterpret_cast<UObject*>(base), fd.boolProp);
                UI::PushStyleColor(ImGuiCol_Text, col);
                if (UI::Checkbox(fd.name.c_str(), &val))
                    WriteBool(reinterpret_cast<UObject*>(base), fd.boolProp, val);
                UI::PopStyleColor();
                if (UI::BeginPopupContextItem("##fctx"))
                {
                    if (UI::MenuItem("Copy value"))
                        UI::SetClipboardText(val ? "true" : "false");
                    UI::EndPopup();
                }
                break;
            }
        case WKind::Int:
            {
                auto* val = GetPropertyPtr<int32>(base, fd.offset);
                UI::PushStyleColor(ImGuiCol_Text, col);
                UI::SetNextItemWidth(130.f);
                UI::DragInt(fd.name.c_str(), val);
                UI::PopStyleColor();
                if (UI::BeginPopupContextItem("##fctx"))
                {
                    char buf[32]; snprintf(buf, sizeof(buf), "%d", *val);
                    if (UI::MenuItem("Copy value")) UI::SetClipboardText(buf);
                    UI::EndPopup();
                }
                break;
            }
        case WKind::Float:
            {
                auto* val = GetPropertyPtr<float>(base, fd.offset);
                UI::PushStyleColor(ImGuiCol_Text, col);
                UI::SetNextItemWidth(130.f);
                UI::DragFloat(fd.name.c_str(), val, 0.1f);
                UI::PopStyleColor();
                if (UI::BeginPopupContextItem("##fctx"))
                {
                    char buf[32]; snprintf(buf, sizeof(buf), "%g", *val);
                    if (UI::MenuItem("Copy value")) UI::SetClipboardText(buf);
                    UI::EndPopup();
                }
                break;
            }
        case WKind::Double:
            {
                auto* val = GetPropertyPtr<double>(base, fd.offset);
                UI::PushStyleColor(ImGuiCol_Text, col);
                UI::SetNextItemWidth(130.f);
                UI::DragScalar(fd.name.c_str(), ImGuiDataType_Double, val, 0.1f);
                UI::PopStyleColor();
                if (UI::BeginPopupContextItem("##fctx"))
                {
                    char buf[32]; snprintf(buf, sizeof(buf), "%g", *val);
                    if (UI::MenuItem("Copy value")) UI::SetClipboardText(buf);
                    UI::EndPopup();
                }
                break;
            }
        case WKind::Name:
            {
                const std::string val = GetPropertyPtr<FName>(base, fd.offset)->ToString();
                UI::PushStyleColor(ImGuiCol_Text, col);
                UI::TextUnformatted(fd.name.c_str());
                UI::PopStyleColor();
                UI::SameLine();
                std::string committed;
                if (EditTextField("##nameedit", base + fd.offset, val, committed))
                    EnqueuePropWrite(base, fd.prop, fd.name, committed);
                if (UI::BeginPopupContextItem("##fctx"))
                {
                    if (UI::MenuItem("Copy value")) UI::SetClipboardText(val.c_str());
                    UI::EndPopup();
                }
                break;
            }
        case WKind::Object:
            {
                auto* ptr = *GetPropertyPtr<UObject*>(base, fd.offset);
                const bool valid = ptr && IsValidRaw(ptr);
                UI::PushStyleColor(ImGuiCol_Text, col);
                if (valid)
                    UI::Text("%s = %s'%s'", fd.name.c_str(),
                        ptr->Class ? ptr->Class->GetName().c_str() : "?",
                        ptr->GetName().c_str());
                else
                    UI::Text("%s = nullptr", fd.name.c_str());
                UI::PopStyleColor();
                if (UI::BeginPopupContextItem("##fctx"))
                {
                    if (valid)
                    {
                        if (UI::MenuItem("Jump to object")) s_treeJumpAddr = (uint64_t)ptr;
                        char addr[20]; snprintf(addr, sizeof(addr), "0x%llX", (uint64_t)ptr);
                        if (UI::MenuItem("Copy address")) UI::SetClipboardText(addr);
                    }
                    UI::EndPopup();
                }
                if (valid)
                {
                    UI::SameLine();
                    if (UI::SmallButton("->##obj"))
                        s_treeJumpAddr = (uint64_t)ptr;
                }
                break;
            }
        case WKind::MapOrSet:
        case WKind::Generic:
            {
                const std::string valStr = GetFieldValueAsString(base, fd.field);
                UI::PushStyleColor(ImGuiCol_Text, col);
                UI::Text("%s = %s", fd.name.c_str(), valStr.c_str());
                UI::PopStyleColor();
                if (UI::BeginPopupContextItem("##fctx"))
                {
                    if (UI::MenuItem("Copy value")) UI::SetClipboardText(valStr.c_str());
                    UI::EndPopup();
                }
                break;
            }
        }
    }

    static void DrawStructCached(uintptr_t base, UStruct* ustruct, int depth)
    {
        if (!ustruct || depth > kMaxTreeDepth) return;
        for (const auto& fd : GetOrBuildStructDesc(ustruct))
        {
            UI::PushID(fd.field);
            DrawFieldFromDesc(base, fd, depth);
            UI::PopID();
        }
    }

    static void DrawFieldNode(uintptr_t base, FField* field, const char* label, int depth)
    {
        if (!field) return;
        const ImVec4 col = FieldDepthColor(depth);

        // ── Struct: collapsible subtree ───────────────────────────────────────
        if (auto* sp = FieldCast::Cast<FStructProperty>(field))
        {
            const std::string sname = sp->Struct ? sp->Struct->GetName() : "?";
            if (depth >= kMaxTreeDepth)
            {
                UI::PushStyleColor(ImGuiCol_Text, col);
                UI::Text("%s = %s", label, GetFieldValueAsString(base, field).c_str());
                UI::PopStyleColor();
                return;
            }
            const std::string head = std::string(label) + " (" + sname + ") = "
                                   + GetFieldValueAsString(base, field);
            UI::PushStyleColor(ImGuiCol_Text, col);
            const bool open = UI::TreeNodeEx(label, ImGuiTreeNodeFlags_None, "%s", head.c_str());
            UI::PopStyleColor();
            if (open)
            {
                if (sp->Struct) DrawStructCached(base + sp->Offset, sp->Struct, depth + 1);
                UI::TreePop();
            }
            return;
        }

        // ── Array: collapsible subtree ────────────────────────────────────────
        if (auto* ap = FieldCast::Cast<FArrayProperty>(field))
        {
            const auto& arr  = *GetPropertyPtr<UC::TArray<uint8>>(base, ap->Offset);
            const int32 num  = arr.Num();
            const std::string tname = (ap->InnerProperty && ap->InnerProperty->ClassPrivate)
                ? ap->InnerProperty->ClassPrivate->Name.ToString() : "?";
            const std::string head = std::string(label)
                + " [" + std::to_string(num) + "] <" + tname + ">";
            UI::PushStyleColor(ImGuiCol_Text, col);
            const bool open = UI::TreeNodeEx(label, ImGuiTreeNodeFlags_None, "%s", head.c_str());
            UI::PopStyleColor();
            if (open)
            {
                if (arr.IsValid() && num > 0 && ap->InnerProperty && depth < kMaxTreeDepth)
                {
                    const int32     show = std::min(num, kMaxArrShow);
                    const int32     es   = ap->InnerProperty->ElementSize;
                    const uintptr_t db   = reinterpret_cast<uintptr_t>(arr.GetDataPtr());
                    for (int32 i = 0; i < show; ++i)
                    {
                        char lbl[32];
                        snprintf(lbl, sizeof(lbl), "[%d]", i);
                        UI::PushID(i);
                        DrawFieldNode(db + (uintptr_t)i * es, ap->InnerProperty, lbl, depth + 1);
                        UI::PopID();
                    }
                    if (num > show) UI::TextDisabled("  ... %d more", num - show);
                }
                UI::TreePop();
            }
            return;
        }

        // ── Bool: inline editable checkbox ───────────────────────────────────
        if (auto* bp = FieldCast::Cast<FBoolProperty>(field))
        {
            // ReadBool/WriteBool use the pointer purely as a base address for offset
            // arithmetic — safe to call with any struct interior cast to UObject*.
            bool val = ReadBool(reinterpret_cast<UObject*>(base), bp);
            UI::PushStyleColor(ImGuiCol_Text, col);
            if (UI::Checkbox(label, &val))
                WriteBool(reinterpret_cast<UObject*>(base), bp, val);
            UI::PopStyleColor();
            if (UI::BeginPopupContextItem("##fctx"))
            {
                if (UI::MenuItem("Copy value")) UI::SetClipboardText(val ? "true" : "false");
                UI::EndPopup();
            }
            return;
        }

        // ── Map / Set: read-only text (container editing not yet supported) ──
        if (FieldCast::IsA<FMapProperty>(field) || FieldCast::IsA<FSetProperty>(field))
        {
            UI::PushStyleColor(ImGuiCol_Text, col);
            UI::Text("%s = %s", label, GetFieldValueAsString(base, field).c_str());
            UI::PopStyleColor();
            if (UI::BeginPopupContextItem("##fctx"))
            {
                if (UI::MenuItem("Copy value"))
                    UI::SetClipboardText(GetFieldValueAsString(base, field).c_str());
                UI::EndPopup();
            }
            return;
        }

        // ── Generic leaf: read-only text + copy context menu ─────────────────
        {
            const std::string valStr = GetFieldValueAsString(base, field);
            UI::PushStyleColor(ImGuiCol_Text, col);
            UI::Text("%s = %s", label, valStr.c_str());
            UI::PopStyleColor();
            if (UI::BeginPopupContextItem("##fctx"))
            {
                if (UI::MenuItem("Copy value")) UI::SetClipboardText(valStr.c_str());
                UI::EndPopup();
            }
        }
    }

    // ── Function tree ────────────────────────────────────────────────────────
    // Renders one function using pre-cached FuncDesc — no per-frame allocations.

    static void DrawFunctionNode(UObject* obj, const FuncDesc& fd)
    {
        UI::PushID(fd.func);
        const bool open = UI::TreeNodeEx(fd.func, ImGuiTreeNodeFlags_None,
            "%s%s", fd.name.c_str(), fd.suffix.c_str());
        if (open)
        {
            if (fd.params.empty())
                UI::TextDisabled("  (no parameters)");
            else
                for (const auto& p : fd.params)
                    UI::TextDisabled("  %s %s : %s", p.dir, p.name.c_str(), p.type.c_str());

            if (fd.callable)
            {
                if (UI::SmallButton("Call"))
                {
                    UObject*    captObj   = obj;
                    UFunction*  captFunc  = fd.func;
                    const int32 parmsSize = fd.parmsSize;
                    EnqueueOnce([captObj, captFunc, parmsSize]()
                    {
                        if (!IsValidRaw(captObj)) return;
                        std::vector<uint8> parms(parmsSize > 0 ? (size_t)parmsSize : 1, 0);
                        captObj->ProcessEvent(captFunc, parms.data());
                    });
                }
                UI::SameLine();
                UI::TextDisabled("(dispatched to game thread)");
            }
            else if (fd.hasInputParams)
                UI::TextDisabled("  [input params — direct call not yet supported]");
            else
                UI::TextDisabled("  [not BlueprintCallable / Exec]");

            UI::TreePop();
        }
        UI::PopID();
    }

    static void DrawFunctionSection(UObject* obj, const std::vector<UStruct*>& chain)
    {
        bool anyFuncs = false;
        for (auto* level : chain)
            if (level && !GetOrBuildFuncList(level).empty()) { anyFuncs = true; break; }
        if (!anyFuncs) return;

        UI::Spacing();
        UI::Separator();
        UI::TextDisabled("Functions");
        UI::Spacing();

        UI::PushID("##funcs");
        for (size_t lvl = 0; lvl < chain.size(); ++lvl)
        {
            UStruct* level = chain[lvl];
            if (!level) continue;
            if (!IsValidRaw(level)) continue;
            const auto& funcs = GetOrBuildFuncList(level);
            if (funcs.empty()) continue;

            UI::PushID(level);
            UI::PushStyleColor(ImGuiCol_Text, ClassLevelColor((int)lvl));
            const bool open = UI::TreeNodeEx(level, ImGuiTreeNodeFlags_None,
                "%s", level->GetName().c_str());
            UI::PopStyleColor();
            if (open)
            {
                for (const auto& fd : funcs)
                    DrawFunctionNode(obj, fd);
                UI::TreePop();
            }
            UI::PopID();
        }
        UI::PopID();
    }

    static void DrawObjectTree(UObject* obj, const char* fieldFilter)
    {
        if (!obj || !IsValidRaw(obj) || !obj->Class || !IsValidRaw(obj->Class)) return;

        // Record the root object for inline field writes (consumed by EnqueuePropWrite).
        s_rootObj = obj;
        s_rootIdx = obj->Index;

        // Header: object name + class jump button + outer jump button
        UI::TextUnformatted(obj->GetName().c_str());
        UI::SameLine();
        {
            char cls[256];
            snprintf(cls, sizeof(cls), "[%s]##cls", obj->Class->GetName().c_str());
            if (UI::SmallButton(cls))
                s_treeJumpClassAddr = (uint64_t)obj->Class;
        }
        if (obj->Outer && IsValidRaw(obj->Outer))
        {
            UI::SameLine(); UI::TextDisabled("outer:");
            UI::SameLine();
            char out[256];
            snprintf(out, sizeof(out), "%s##out", obj->Outer->GetName().c_str());
            if (UI::SmallButton(out))
                s_treeJumpAddr = (uint64_t)obj->Outer;
        }
        UI::Separator();

        // Walk all the way to UObject (pass UObject::StaticClass() as stop).
        const auto    chain  = BuildClassChain(obj->Class, UObject::StaticClass());
        const uintptr_t base = reinterpret_cast<uintptr_t>(obj);

        for (size_t lvl = 0; lvl < chain.size(); ++lvl)
        {
            UStruct* level = chain[lvl];
            if (!level || !IsValidRaw(level)) continue;
            const ImGuiTreeNodeFlags flags = (lvl == 0)
                ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
            UI::PushID((int)lvl);
            UI::PushStyleColor(ImGuiCol_Text, ClassLevelColor((int)lvl));
            const bool open = UI::TreeNodeEx(level, flags, "%s", level->GetName().c_str());
            UI::PopStyleColor();
            if (open)
            {
                const auto& desc = GetOrBuildStructDesc(level);
                if (desc.empty())
                {
                    UI::TextDisabled("(no properties)");
                }
                else
                {
                    bool anyShown = false;
                    for (const auto& fd : desc)
                    {
                        if (fieldFilter[0] && !IContains(fd.name, fieldFilter)) continue;
                        anyShown = true;
                        UI::PushID(fd.field);
                        DrawFieldFromDesc(base, fd, 0);
                        UI::PopID();
                    }
                    if (!anyShown && fieldFilter[0])
                        UI::TextDisabled("(no matches)");
                }
                UI::TreePop();
            }
            UI::PopID();
        }

        DrawFunctionSection(obj, chain);
    }

    // ── Tab ─────────────────────────────────────────────────────────────────────

    struct ObjectBrowserTab : detail::OverlayTab<ObjectBrowserTab>
    {
        static constexpr const char* kName = "Objects";

        // Text filters (multi-token, IStartsWithAny)
        char m_fClass[256] = {};
        char m_fName[256]  = {};
        char m_fOuter[128] = {}, m_fOwner[128] = {}, m_fInst[128] = {};

        // Mode selectors
        int  m_classMode = 0;   // 0 Contains, 1 Is, 2 Is+Sub, 3 Not, 4 Not+Sub
        int  m_netMode   = 0;   // 0 All, 1 Replicated, 2 Not-rep, 3 Actors, 4 Non-actors

        // Type visibility checkboxes
        bool m_skipInternals = true;   // hide UClass/UFunction/UEnum/UPackage/…
        bool m_showCDOs      = false;  // show Class Default Objects
        bool m_showBP        = true;   // show instances of Blueprint-compiled classes
        bool m_showNative    = true;   // show instances of native C++ classes

        // Selection / jump
        uint64_t m_selectedAddr        = 0;
        uint64_t m_prevSelected        = 0;
        uint64_t m_gotoAddr            = 0;
        int32    m_selectedIdx = -1;  // UObject::Index recorded at selection; detects GC + reuse

        // Property-panel field filter
        char m_fieldFilter[128] = {};

        // Navigation history (Mouse4 = back, Mouse5 = forward)
        std::vector<uint64_t> m_navHistory;
        size_t                m_navIdx = 0;

        // Footer counts
        size_t m_shown = 0, m_total = 0;

        // Snapshots — version-gated copy from game thread (no timer, copy only when new data)
        std::vector<ObjectList::Row>         m_objRows;
        std::vector<ActorList::Row>          m_actorRows;
        std::unordered_map<uint64_t, size_t> m_actorIdx;   // addr → index into m_actorRows
        uint64_t m_objSnapVer = 0, m_actSnapVer = 0;       // version when we last copied

        // Cached filtered+sorted index — rebuilt only when data or filters change
        std::vector<int> m_sortedIdx;
        uint64_t m_sortedObjVer = 0, m_sortedActVer = 0;   // version when we last rebuilt
        int      m_sortCol      = -1;   // -1 = no sort
        bool     m_sortAsc      = true;
        // Filter state snapshot for dirty detection
        char     m_fcClass[256]{}, m_fcName[256]{};
        char     m_fcOuter[128]{}, m_fcOwner[128]{}, m_fcInst[128]{};
        int      m_fcClassMode = 0,    m_fcNetMode   = 0;
        bool     m_fcSkipInt   = true, m_fcShowCDOs  = false;
        bool     m_fcShowBP    = true, m_fcShowNative = true;

        // ── Class filter ─────────────────────────────────────────────────────────
        // classChain is the actor's slash-wrapped ancestry; empty for non-actors
        // (fallback to IStartsWith on className for Is+Sub / Not+Sub modes).
        bool ClassFilterPass(const std::string& className, const std::string& classChain,
                             const std::vector<std::string>& tokens)
        {
            if (tokens.empty()) return true;
            switch (m_classMode)
            {
            case 0: // Contains any
                for (const auto& t : tokens) if (IContains(className, t)) return true;
                return false;
            case 1: // Equals any
                for (const auto& t : tokens) if (IEquals(className, t)) return true;
                return false;
            case 2: // Is or subclass — chain check for actors, prefix for others
                for (const auto& t : tokens)
                {
                    if (!classChain.empty()) { if (IContains(classChain, "/" + t + "/")) return true; }
                    else                     { if (IStartsWith(className, t))            return true; }
                }
                return false;
            case 3: // Not — exclude if starts with any token
                for (const auto& t : tokens) if (IStartsWith(className, t)) return false;
                return true;
            case 4: // Not+Sub — chain exclusion for actors, prefix exclusion for others
                for (const auto& t : tokens)
                {
                    if (!classChain.empty()) { if (IContains(classChain, "/" + t + "/")) return false; }
                    else                     { if (IStartsWith(className, t))            return false; }
                }
                return true;
            }
            return true;
        }

        // ── Navigation helpers ────────────────────────────────────────────────────

        // Clear filters + set destination without touching history.
        void NavigateTo(uint64_t addr)
        {
            if (!addr) return;
            m_gotoAddr = addr; m_selectedAddr = addr;
            auto* obj = reinterpret_cast<UObject*>(addr);
            m_selectedIdx = IsValidRaw(obj) ? obj->Index : -1;
            m_fClass[0] = m_fName[0] = m_fOuter[0] = m_fOwner[0] = m_fInst[0] = '\0';
            m_netMode   = 0;
            m_showCDOs  = true; m_showBP = true; m_showNative = true;
        }

        // Append addr to the history ring (cap 64), truncating any forward entries.
        void PushHistory(uint64_t addr)
        {
            if (!addr) return;
            if (!m_navHistory.empty() && m_navHistory[m_navIdx] == addr) return;
            if (m_navIdx + 1 < m_navHistory.size())
                m_navHistory.erase(m_navHistory.begin() + (ptrdiff_t)(m_navIdx + 1),
                                   m_navHistory.end());
            m_navHistory.push_back(addr);
            m_navIdx = m_navHistory.size() - 1;
            if (m_navHistory.size() > 64)
            {
                m_navHistory.erase(m_navHistory.begin());
                if (m_navIdx > 0) --m_navIdx;
            }
        }

        // Jump to any object by addr — clears all filters so the target is visible.
        void JumpTo(uint64_t addr)
        {
            NavigateTo(addr);
            PushHistory(addr);
        }

        // Jump to a UClass object — additionally disables skip_internals so it's visible.
        void JumpToClass(uint64_t addr)
        {
            if (!addr) return;
            JumpTo(addr);
            m_skipInternals = false;
        }

        // ── Main draw ────────────────────────────────────────────────────────────
        void Draw()
        {
            // Mouse4 = back, Mouse5 = forward through navigation history
            if (UI::IsMouseClicked(3) && m_navIdx > 0)
            {
                --m_navIdx;
                NavigateTo(m_navHistory[m_navIdx]);
            }
            else if (UI::IsMouseClicked(4) && !m_navHistory.empty()
                     && m_navIdx + 1 < m_navHistory.size())
            {
                ++m_navIdx;
                NavigateTo(m_navHistory[m_navIdx]);
            }

            auto& actSnap = detail::Actors();
            auto& objSnap = detail::Objects();
            const uint64_t now = NowMs();
            actSnap.beat(now);
            objSnap.beat(now);

            // ── Row 1: refresh + type visibility ─────────────────────────────────
            if (UI::Button("Scan")) { objSnap.request(); actSnap.request(); }
            UI::SameLine();
            {
                bool autoOn = objSnap.isAuto();
                if (UI::Checkbox("Auto", &autoOn))
                    { objSnap.setAuto(autoOn); actSnap.setAuto(autoOn); }
            }
            UI::SameLine();
            UI::Checkbox("Skip internals", &m_skipInternals);
            UI::SameLine();
            UI::TextDisabled("|"); UI::SameLine();
            UI::Text("Show:"); UI::SameLine();
            UI::Checkbox("CDOs",    &m_showCDOs);   UI::SameLine();
            UI::Checkbox("BP",      &m_showBP);      UI::SameLine();
            UI::Checkbox("C++",     &m_showNative);
            UI::SameLine();
            UI::TextDisabled("(right-click header to show/hide columns)");

            // ── Row 2: class + name + net ─────────────────────────────────────────
            UI::SetNextItemWidth(90.f);
            UI::Combo("##cmode", &m_classMode, "contains\0is\0is+sub\0not\0not+sub\0");
            UI::SameLine(); UI::SetNextItemWidth(190.f);
            UI::InputTextWithHint("##fclass", "class (space/comma = OR)", m_fClass, sizeof(m_fClass));
            UI::SameLine(); UI::SetNextItemWidth(190.f);
            UI::InputTextWithHint("##fname",  "name  (space/comma = OR)", m_fName,  sizeof(m_fName));
            UI::SameLine(); UI::SetNextItemWidth(130.f);
            UI::Combo("##net", &m_netMode,
                      "net: all\0replicated\0not replicated\0actors\0non-actors\0");

            // ── Row 3: actor-field filters ────────────────────────────────────────
            UI::SetNextItemWidth(155.f);
            UI::InputTextWithHint("##fouter", "outer", m_fOuter, sizeof(m_fOuter));
            UI::SameLine(); UI::SetNextItemWidth(155.f);
            UI::InputTextWithHint("##fowner", "owner", m_fOwner, sizeof(m_fOwner));
            UI::SameLine(); UI::SetNextItemWidth(-1.f);
            UI::InputTextWithHint("##finst",  "instigator", m_fInst, sizeof(m_fInst));

            // ── Version-gated snapshot copy — copy only when game thread has new data ──
            {
                const uint64_t ov = objSnap.version(), av = actSnap.version();
                if (ov != m_objSnapVer || av != m_actSnapVer)
                {
                    m_objRows   = objSnap.read();
                    m_actorRows = actSnap.read();
                    m_objSnapVer = ov; m_actSnapVer = av;
                    m_actorIdx.clear();
                    m_actorIdx.reserve(m_actorRows.size());
                    for (size_t i = 0; i < m_actorRows.size(); ++i)
                        m_actorIdx[m_actorRows[i].addr] = i;
                }
            }
            if (m_objRows.empty()) { objSnap.request(); actSnap.request(); }

            // ── Dirty check — only rebuild when data or any filter changes ────────
            bool needRebuild = (m_objSnapVer != m_sortedObjVer || m_actSnapVer != m_sortedActVer)
                || (strcmp(m_fClass, m_fcClass) != 0)
                || (strcmp(m_fName,  m_fcName)  != 0)
                || (strcmp(m_fOuter, m_fcOuter) != 0)
                || (strcmp(m_fOwner, m_fcOwner) != 0)
                || (strcmp(m_fInst,  m_fcInst)  != 0)
                || (m_classMode     != m_fcClassMode)
                || (m_netMode       != m_fcNetMode)
                || (m_skipInternals != m_fcSkipInt)
                || (m_showCDOs      != m_fcShowCDOs)
                || (m_showBP        != m_fcShowBP)
                || (m_showNative    != m_fcShowNative);

            // ── Outer split: list | property tree — resizable ─────────────────────
            const float splitH = UI::GetContentRegionAvail().y;
            const ImGuiTableFlags splitTf = ImGuiTableFlags_BordersOuter
                                          | ImGuiTableFlags_BordersInnerV
                                          | ImGuiTableFlags_Resizable;
            if (!UI::BeginTable("##split", 2, splitTf, ImVec2(0.f, splitH))) return;
            UI::TableSetupColumn("##listcol", ImGuiTableColumnFlags_WidthStretch, 0.45f);
            UI::TableSetupColumn("##treecol", ImGuiTableColumnFlags_WidthStretch, 0.55f);
            UI::TableNextRow();

            // ── Left pane: unified object list + footer ───────────────────────────
            UI::TableSetColumnIndex(0);
            if (UI::BeginChild("##listpane", ImVec2(0.f, 0.f)))
            {
                const float footerH = UI::GetTextLineHeightWithSpacing()
                                    + UI::GetStyle().ItemSpacing.y;
                const ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
                                         | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
                                         | ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable
                                         | ImGuiTableFlags_Hideable;
                if (UI::BeginTable("##objs", 7, tf, ImVec2(0.f, -footerH)))
                {
                    UI::TableSetupColumn("class",      ImGuiTableColumnFlags_WidthStretch, 0, 0);
                    UI::TableSetupColumn("name",       ImGuiTableColumnFlags_WidthStretch, 0, 1);
                    UI::TableSetupColumn("net",        ImGuiTableColumnFlags_WidthFixed,  40, 2);
                    UI::TableSetupColumn("outer",      ImGuiTableColumnFlags_WidthStretch, 0, 3);
                    UI::TableSetupColumn("owner",      ImGuiTableColumnFlags_WidthStretch, 0, 4);
                    UI::TableSetupColumn("instigator", ImGuiTableColumnFlags_WidthStretch, 0, 5);
                    UI::TableSetupColumn("addr",
                        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 120, 6);
                    UI::TableSetupScrollFreeze(0, 1);
                    UI::TableHeadersRow();

                    // Update cached sort direction when the user clicks a column header.
                    if (auto* ss = UI::TableGetSortSpecs(); ss && ss->SpecsDirty)
                    {
                        if (ss->SpecsCount > 0) {
                            m_sortCol = ss->Specs[0].ColumnUserID;
                            m_sortAsc = (ss->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
                        } else {
                            m_sortCol = -1;
                        }
                        ss->SpecsDirty = false;
                        needRebuild = true;
                    }

                    // Rebuild filtered+sorted index only when something changed.
                    if (needRebuild)
                    {
                        const auto classTokens = ParseTokens(m_fClass);
                        const auto nameTokens  = ParseTokens(m_fName);
                        const auto outerTokens = ParseTokens(m_fOuter);
                        const auto ownerTokens = ParseTokens(m_fOwner);
                        const auto instTokens  = ParseTokens(m_fInst);

                        m_sortedIdx.clear();
                        for (int i = 0; i < (int)m_objRows.size(); ++i)
                        {
                            const ObjectList::Row& r  = m_objRows[i];
                            auto                   it = m_actorIdx.find(r.addr);
                            const bool             isActor = (it != m_actorIdx.end());
                            const ActorList::Row*  a  = isActor ? &m_actorRows[it->second] : nullptr;

                            if (m_skipInternals && r.isInternal)        continue;
                            if (!m_showCDOs     && r.isDefault)         continue;
                            if (!m_showBP       && r.isBP)              continue;
                            if (!m_showNative   && !r.isBP)             continue;

                            switch (m_netMode)
                            {
                            case 1: if (!isActor || !a->replicated) continue; break;
                            case 2: if (!isActor ||  a->replicated) continue; break;
                            case 3: if (!isActor)                   continue; break;
                            case 4: if  (isActor)                   continue; break;
                            }

                            const std::string& chain    = a ? a->classChain : std::string{};
                            const std::string& outerStr = a ? a->outer      : r.outer;
                            const std::string& ownerStr = a ? a->owner      : std::string{};
                            const std::string& instStr  = a ? a->instigator : std::string{};

                            if (!ClassFilterPass(r.className, chain, classTokens)) continue;
                            if (!IStartsWithAny(r.name,    nameTokens))            continue;
                            if (!IStartsWithAny(outerStr,  outerTokens))           continue;
                            if (!IStartsWithAny(ownerStr,  ownerTokens))           continue;
                            if (!IStartsWithAny(instStr,   instTokens))            continue;

                            m_sortedIdx.push_back(i);
                        }

                        if (m_sortCol >= 0)
                        {
                            std::sort(m_sortedIdx.begin(), m_sortedIdx.end(), [&](int a, int b)
                            {
                                const ObjectList::Row& ra = m_objRows[a];
                                const ObjectList::Row& rb = m_objRows[b];
                                auto ia = m_actorIdx.find(ra.addr);
                                auto ib = m_actorIdx.find(rb.addr);
                                const ActorList::Row* aa = (ia != m_actorIdx.end()) ? &m_actorRows[ia->second] : nullptr;
                                const ActorList::Row* ab = (ib != m_actorIdx.end()) ? &m_actorRows[ib->second] : nullptr;
                                int c = 0;
                                switch (m_sortCol)
                                {
                                case 0: c = ra.className.compare(rb.className); break;
                                case 1: c = ra.name.compare(rb.name);           break;
                                case 2:
                                    c = (int)(aa ? aa->replicated : false)
                                      - (int)(ab ? ab->replicated : false); break;
                                case 3:
                                {
                                    const std::string& oa = aa ? aa->outer : ra.outer;
                                    const std::string& ob = ab ? ab->outer : rb.outer;
                                    c = oa.compare(ob);
                                } break;
                                case 4:
                                {
                                    const std::string& oa = aa ? aa->owner : std::string{};
                                    const std::string& ob = ab ? ab->owner : std::string{};
                                    c = oa.compare(ob);
                                } break;
                                case 5:
                                {
                                    const std::string& oa = aa ? aa->instigator : std::string{};
                                    const std::string& ob = ab ? ab->instigator : std::string{};
                                    c = oa.compare(ob);
                                } break;
                                case 6:
                                    c = (ra.addr < rb.addr) ? -1 : (ra.addr > rb.addr ? 1 : 0); break;
                                }
                                return m_sortAsc ? c < 0 : c > 0;
                            });
                        }

                        m_sortedObjVer = m_objSnapVer;
                        m_sortedActVer = m_actSnapVer;
                        m_fcClassMode  = m_classMode;
                        m_fcNetMode    = m_netMode;
                        m_fcSkipInt    = m_skipInternals;
                        m_fcShowCDOs   = m_showCDOs;
                        m_fcShowBP     = m_showBP;
                        m_fcShowNative = m_showNative;
                        memcpy(m_fcClass, m_fClass, sizeof(m_fClass));
                        memcpy(m_fcName,  m_fName,  sizeof(m_fName));
                        memcpy(m_fcOuter, m_fOuter, sizeof(m_fOuter));
                        memcpy(m_fcOwner, m_fOwner, sizeof(m_fOwner));
                        memcpy(m_fcInst,  m_fInst,  sizeof(m_fInst));
                    }

                    m_shown = m_sortedIdx.size();
                    m_total = m_objRows.size();

                    // Resolve pending jump-to position in filtered list.
                    int gotoPos = -1;
                    if (m_gotoAddr)
                        for (int k = 0; k < (int)m_sortedIdx.size(); ++k)
                            if (m_objRows[m_sortedIdx[k]].addr == m_gotoAddr) { gotoPos = k; break; }

                    ImGuiListClipper clipper;
                    clipper.Begin((int)m_sortedIdx.size());
                    if (gotoPos >= 0) clipper.IncludeItemByIndex(gotoPos);
                    while (clipper.Step())
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                        {
                            const int             objI = m_sortedIdx[row];
                            const ObjectList::Row& r   = m_objRows[objI];

                            auto it = m_actorIdx.find(r.addr);
                            const bool isActor = (it != m_actorIdx.end());
                            const ActorList::Row* a = isActor ? &m_actorRows[it->second] : nullptr;

                            UI::TableNextRow();
                            UI::PushID(objI);

                            const bool sel = (r.addr && r.addr == m_selectedAddr);
                            if (sel) UI::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                                         IM_COL32(60, 90, 130, 120));

                            // col 0 — class (button → jump to UClass object)
                            UI::TableSetColumnIndex(0);
                            if (r.classAddr)
                            {
                                if (UI::SmallButton((r.className + "##cls").c_str()))
                                    JumpToClass(r.classAddr);
                            }
                            else UI::TextUnformatted(r.className.c_str());

                            // col 1 — name (selects object for property tree)
                            UI::TableSetColumnIndex(1);
                            if (UI::Selectable(r.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
                            {
                                m_selectedAddr = r.addr;
                                auto* selObj = reinterpret_cast<UObject*>(r.addr);
                                m_selectedIdx = IsValidRaw(selObj) ? selObj->Index : -1;
                                PushHistory(r.addr);
                            }

                            // col 2 — net replication status
                            UI::TableSetColumnIndex(2);
                            if (a && a->replicated)
                                UI::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.f), "rep");
                            else
                                UI::TextDisabled("-");

                            // col 3 — outer (button → jump; works for actors via outerAddr,
                            //          and for all objects via r.outerAddr populated at snapshot)
                            UI::TableSetColumnIndex(3);
                            {
                                const uint64_t outerA = a ? a->outerAddr : r.outerAddr;
                                const std::string& outerS = a ? a->outer : r.outer;
                                if (outerA)
                                    { if (UI::SmallButton((outerS + "##o").c_str())) JumpTo(outerA); }
                                else if (!outerS.empty())
                                    UI::TextUnformatted(outerS.c_str());
                                else
                                    UI::TextDisabled("-");
                            }

                            // col 4 — owner (actors only)
                            UI::TableSetColumnIndex(4);
                            if (a && a->ownerAddr)
                                { if (UI::SmallButton((a->owner + "##w").c_str())) JumpTo(a->ownerAddr); }
                            else if (a && !a->owner.empty())
                                UI::TextUnformatted(a->owner.c_str());
                            else
                                UI::TextDisabled("-");

                            // col 5 — instigator (actors only)
                            UI::TableSetColumnIndex(5);
                            if (a && a->instigatorAddr)
                                { if (UI::SmallButton((a->instigator + "##i").c_str())) JumpTo(a->instigatorAddr); }
                            else if (a && !a->instigator.empty())
                                UI::TextUnformatted(a->instigator.c_str());
                            else
                                UI::TextDisabled("-");

                            // col 6 — raw pointer address
                            UI::TableSetColumnIndex(6);
                            UI::Text("%llX", (unsigned long long)r.addr);

                            if (gotoPos == row) UI::SetScrollHereY(0.5f);
                            UI::PopID();
                        }
                    if (gotoPos >= 0) m_gotoAddr = 0;

                    UI::EndTable();
                }
                UI::TextDisabled("%zu shown / %zu objects  (%zu actors)",
                    m_shown, m_total, m_actorRows.size());
            }
            UI::EndChild();

            // ── Right pane: property tree ────────────────────────────────────────
            UI::TableSetColumnIndex(1);
            const bool newSel = (m_selectedAddr != m_prevSelected);
            if (UI::BeginChild("##proptree", ImVec2(0.f, 0.f)))
            {
                if (newSel) UI::SetScrollHereY(0.f);
                if (m_selectedAddr)
                {
                    auto* obj = reinterpret_cast<UObject*>(m_selectedAddr);
                    if (!IsValidRaw(obj) || obj->Index != m_selectedIdx)
                    {
                        UI::TextDisabled("(object destroyed or replaced — deselected)");
                        m_selectedAddr = 0;
                    }
                    else
                    {
                        UI::SetNextItemWidth(-1.f);
                        UI::InputTextWithHint("##ffilter", "filter fields...",
                                             m_fieldFilter, sizeof(m_fieldFilter));
                        DrawObjectTree(obj, m_fieldFilter);
                        if (s_treeJumpAddr)
                        {
                            JumpTo(s_treeJumpAddr);
                            s_treeJumpAddr = 0;
                        }
                        if (s_treeJumpClassAddr)
                        {
                            JumpToClass(s_treeJumpClassAddr);
                            s_treeJumpClassAddr = 0;
                        }
                    }
                }
                else
                    UI::TextDisabled("Select an object from the list on the left.");
            }
            UI::EndChild();
            m_prevSelected = m_selectedAddr;

            UI::EndTable();
        }
    };

} // anonymous namespace

    void detail::RegisterObjectsTab() { static ObjectBrowserTab s; (void)s; }
}
