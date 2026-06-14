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
#include "../../game/Lib_ObjectView.h"   // ObjView model + WritePath (game-thread renderer model)
#include "../../game/Lib_Print.h"
#include "../../hooks/Lib_GameHooks.h"

#include <imgui.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
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
    // Set by Object-field "→" buttons / class-outer buttons; consumed by the tab each frame.
    static uint64_t s_treeJumpAddr      = 0;
    static uint64_t s_treeJumpClassAddr = 0;

    // ── Inline field editing ──────────────────────────────────────────────────────
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



    // ── Function calling ─────────────────────────────────────────────────────────
    // Per-function input-arg text buffers (UI thread only; one string per input param).
    static std::unordered_map<UFunction*, std::vector<std::string>> s_callArgs;
    // Last call result per function (return + out params, formatted). Written on the game
    // thread by InvokeFunctionGameThread, read on the UI thread by RenderModelFunc.
    static std::mutex s_callResMx;
    static std::unordered_map<UFunction*, std::string> s_callResults;

    static bool ParamIsInput(const char* dir) { return std::string_view(dir).find("in") != std::string_view::npos; }

    // Marshal `inArgs` into a parm buffer, ProcessEvent on the GAME THREAD, then read the
    // return + out params back as strings and stash them in s_callResults. Mirrors the CLI
    // `call` command's marshaling (ComputeParmsSize/WriteParam, Native-flag toggle, FString/
    // FName destruct) and additionally captures the output the command path discards.
    static void InvokeFunctionGameThread(UObject* obj, int32 objIdx, UFunction* func,
                                         std::vector<std::string> inArgs)
    {
        auto setResult = [&](std::string s)
        {
            std::lock_guard<std::mutex> lk(s_callResMx);
            s_callResults[func] = std::move(s);
        };
        if (!IsValidRaw(obj) || obj->Index != objIdx || !IsValidRaw(func))
        { setResult("(object/function no longer valid)"); return; }

        std::vector<FProperty*> inParms;    // consume args, in declaration order
        std::vector<FProperty*> outParms;   // out + return — read back after the call
        FProperty* retProp = nullptr;
        for (FField* f : FFieldRange(func->ChildProperties))
        {
            auto* p = FieldCast::Cast<FProperty>(f);
            if (!p) continue;
            const auto pf = static_cast<EPropertyFlags>(p->PropertyFlags);
            if (!(pf & EPropertyFlags::Parm)) continue;
            // A const-reference parameter carries CPF_OutParm too, but it's an INPUT passed by
            // ref — it must be filled from args, not left zeroed (a null ref → ProcessEvent
            // deref crash). Only OutParm WITHOUT ConstParm is a real output.
            if (pf & EPropertyFlags::ReturnParm) { retProp = p; outParms.push_back(p); }
            else if ((pf & EPropertyFlags::OutParm) && !(pf & EPropertyFlags::ConstParm)) outParms.push_back(p);
            else inParms.push_back(p);
        }

        const int32 parmsSize = PropertyInspector::ComputeParmsSize(func);
        std::vector<uint8> buf(parmsSize > 0 ? (size_t)parmsSize : 1, 0);
        for (size_t i = 0; i < inParms.size() && i < inArgs.size(); ++i)
            if (!inArgs[i].empty())
                PropertyInspector::WriteParam(inParms[i], inArgs[i], buf.data());

        const uint32 savedFlags = func->FunctionFlags;
        if (static_cast<EFunctionFlags>(func->FunctionFlags) & EFunctionFlags::Native)
            func->FunctionFlags |= 0x400;
        obj->ProcessEvent(func, parmsSize > 0 ? buf.data() : nullptr);
        func->FunctionFlags = savedFlags;

        const uintptr_t bb = reinterpret_cast<uintptr_t>(buf.data());
        std::string result;
        for (FProperty* p : outParms)   // read BEFORE destructing any strings below
        {
            if (!result.empty()) result += "   ";
            result += (p == retProp ? std::string("return") : p->Name.ToString())
                    + " = " + GetFieldValueAsString(bb, p);
        }
        if (outParms.empty()) result = "(called — no return/out value)";

        auto destruct = [&](FProperty* p)
        {
            FieldCast::Visit(p, [&]<typename T>(T* pp)
            {
                if      constexpr (std::is_same_v<T, FStrProperty>)  GetPropertyPtr<FString>(bb, pp->Offset)->~FString();
                else if constexpr (std::is_same_v<T, FNameProperty>) GetPropertyPtr<FName>(bb, pp->Offset)->~FName();
            });
        };
        for (FProperty* p : inParms)  destruct(p);
        for (FProperty* p : outParms) destruct(p);

        setResult(std::move(result));
    }


    // ════════════════════════════════════════════════════════════════════════════
    // Model renderer — consumes ObjView::ObjectView (a game-thread snapshot). Touches NO live
    // game memory: every value is a string/number already extracted game-side; writes/jumps go
    // back through opaque PropPath / addresses. This is what replaces the live-read tree above.
    // ════════════════════════════════════════════════════════════════════════════

    static void EnqueuePathWrite(const ObjView::PropPath& path, std::string value)
    {
        EnqueueOnce([path, value]() { ObjView::WritePath(path, value); });
    }

    // Active-drag guard: while the user drags a numeric/vector widget keep the in-progress value;
    // otherwise mirror the model (which the producer refreshes every tick). One active drag.
    static uint64_t s_dragKey = 0;
    static double   s_dragVal[3] = {};

    static void RenderScalarDrag(const ObjView::PropNode& n, const ImVec4& col)
    {
        const bool active = (s_dragKey == n.key);
        UI::PushStyleColor(ImGuiCol_Text, col);
        UI::SetNextItemWidth(140.f);
        bool changed = false;
        char buf[40];
        if (n.edit == ObjView::EditKind::Int)
        {
            int v = active ? (int)s_dragVal[0] : atoi(n.valueStr.c_str());
            changed = UI::DragInt(n.name.c_str(), &v);
            if (UI::IsItemActivated()) s_dragKey = n.key;
            if (s_dragKey == n.key) { s_dragVal[0] = v; if (changed) { snprintf(buf, sizeof buf, "%d", v); EnqueuePathWrite(n.path, buf); } }
        }
        else
        {
            double v = active ? s_dragVal[0] : strtod(n.valueStr.c_str(), nullptr);
            float f = (float)v;
            changed = UI::DragFloat(n.name.c_str(), &f, 0.1f);
            if (UI::IsItemActivated()) s_dragKey = n.key;
            if (s_dragKey == n.key) { s_dragVal[0] = f; if (changed) { snprintf(buf, sizeof buf, "%g", (double)f); EnqueuePathWrite(n.path, buf); } }
        }
        if (s_dragKey == n.key && UI::IsItemDeactivated()) s_dragKey = 0;
        UI::PopStyleColor();
    }

    static void RenderVec3(const ObjView::PropNode& n, const ImVec4& col)
    {
        const bool active = (s_dragKey == n.key);
        float v[3] = { n.vec3[0], n.vec3[1], n.vec3[2] };
        if (active) { v[0] = (float)s_dragVal[0]; v[1] = (float)s_dragVal[1]; v[2] = (float)s_dragVal[2]; }
        UI::PushStyleColor(ImGuiCol_Text, col);
        UI::TextUnformatted(n.name.c_str());
        UI::PopStyleColor();
        UI::SameLine();
        UI::SetNextItemWidth(240.f);
        const bool changed = UI::DragFloat3("##vec3", v, 0.1f);
        if (UI::IsItemActivated()) s_dragKey = n.key;
        if (s_dragKey == n.key)
        {
            s_dragVal[0] = v[0]; s_dragVal[1] = v[1]; s_dragVal[2] = v[2];
            if (changed) { char b[96]; snprintf(b, sizeof b, "%g,%g,%g", v[0], v[1], v[2]); EnqueuePathWrite(n.path, b); }
            if (UI::IsItemDeactivated()) s_dragKey = 0;
        }
    }

    static void RenderEnum(const ObjView::PropNode& n, const ImVec4& col)
    {
        UI::PushStyleColor(ImGuiCol_Text, col);
        UI::TextUnformatted(n.name.c_str());
        UI::PopStyleColor();
        UI::SameLine();
        if (n.enumNames.empty()) { UI::Text("= %lld", (long long)n.enumValue); return; }
        int idx = -1;
        for (int i = 0; i < (int)n.enumValues.size(); ++i)
            if (n.enumValues[i] == n.enumValue) { idx = i; break; }
        UI::SetNextItemWidth(220.f);
        if (UI::ComboFromList("##enum", &idx, n.enumNames, n.enumNames.size() > 12, {}, nullptr, false)
            && idx >= 0 && idx < (int)n.enumValues.size())
            EnqueuePathWrite(n.path, std::to_string(n.enumValues[idx]));
    }

    static void RenderModelNode(const ObjView::PropNode& n, std::set<uint64_t>& expanded)
    {
        const ImVec4 col = FieldDepthColor(n.depth);
        UI::PushID(reinterpret_cast<void*>(static_cast<uintptr_t>(n.key)));

        if (n.hasChildren)   // struct / array — expandable
        {
            char head[400];
            if (n.container == ObjView::Container::Array)
                snprintf(head, sizeof head, "%s [%d] <%s>", n.name.c_str(), n.containerNum, n.typeName.c_str());
            else
                snprintf(head, sizeof head, "%s (%s) = %s", n.name.c_str(), n.typeName.c_str(), n.valueStr.c_str());
            UI::PushStyleColor(ImGuiCol_Text, col);
            const bool open = UI::TreeNodeEx(head, ImGuiTreeNodeFlags_None);
            UI::PopStyleColor();
            // Mirror ImGui's open-state into the request set — the producer expands matching keys.
            if (open) expanded.insert(n.key); else expanded.erase(n.key);
            if (open)
            {
                if (n.children.empty()) UI::TextDisabled("  …");   // requested; fills in next tick
                else for (const auto& c : n.children) RenderModelNode(c, expanded);
                UI::TreePop();
            }
            UI::PopID();
            return;
        }

        switch (n.edit)
        {
        case ObjView::EditKind::Bool:
        {
            bool b = n.boolVal;
            UI::PushStyleColor(ImGuiCol_Text, col);
            if (UI::Checkbox(n.name.c_str(), &b)) EnqueuePathWrite(n.path, b ? "true" : "false");
            UI::PopStyleColor();
            break;
        }
        case ObjView::EditKind::Int:
        case ObjView::EditKind::Float:
        case ObjView::EditKind::Double:
            RenderScalarDrag(n, col);
            break;
        case ObjView::EditKind::Name:
        case ObjView::EditKind::Str:
        case ObjView::EditKind::Text:
        {
            UI::PushStyleColor(ImGuiCol_Text, col);
            UI::TextUnformatted(n.name.c_str());
            UI::PopStyleColor();
            UI::SameLine();
            std::string committed;
            if (EditTextField("##e", static_cast<uintptr_t>(n.key), n.valueStr, committed))
                EnqueuePathWrite(n.path, committed);
            break;
        }
        case ObjView::EditKind::Enum:
            RenderEnum(n, col);
            break;
        case ObjView::EditKind::VectorLike:
            RenderVec3(n, col);
            break;
        case ObjView::EditKind::Object:
        {
            UI::PushStyleColor(ImGuiCol_Text, col);
            UI::Text("%s = %s", n.name.c_str(), n.valueStr.c_str());
            UI::PopStyleColor();
            if (n.objectAddr) { UI::SameLine(); if (UI::SmallButton("->")) s_treeJumpAddr = n.objectAddr; }
            if (UI::BeginPopupContextItem("##oc"))
            {
                if (n.objectAddr && UI::MenuItem("Jump to object")) s_treeJumpAddr = n.objectAddr;
                if (UI::MenuItem("Copy value")) UI::SetClipboardText(n.valueStr.c_str());
                UI::EndPopup();
            }
            break;
        }
        case ObjView::EditKind::ReadOnly:
        default:
        {
            UI::PushStyleColor(ImGuiCol_Text, col);
            UI::Text("%s = %s", n.name.c_str(), n.valueStr.c_str());
            UI::PopStyleColor();
            if (UI::BeginPopupContextItem("##rc"))
            {
                if (UI::MenuItem("Copy value")) UI::SetClipboardText(n.valueStr.c_str());
                UI::EndPopup();
            }
            break;
        }
        }
        // Type annotation next to the value (expandable struct/array nodes return early above and
        // already show their type in the header).
        if (!n.typeName.empty()) { UI::SameLine(); UI::TextDisabled(": %s", n.typeName.c_str()); }
        UI::PopID();
    }

    // One function node, calling on the focused object (addr+idx) by funcId. Reuses the existing
    // game-thread invoke + s_callArgs/s_callResults stores (funcId IS the UFunction*).
    static void RenderModelFunc(uint64_t objAddr, int32 objIdx, const ObjView::FuncView& fv)
    {
        auto* func = reinterpret_cast<UFunction*>(fv.funcId);
        UI::PushID(reinterpret_cast<void*>(static_cast<uintptr_t>(fv.funcId)));
        const bool open = UI::TreeNodeEx(func, ImGuiTreeNodeFlags_None, "%s%s", fv.name.c_str(), fv.suffix.c_str());
        if (open)
        {
            if (fv.params.empty()) UI::TextDisabled("  (no parameters)");
            auto& argBuf = s_callArgs[func];
            size_t inIdx = 0;
            for (const auto& p : fv.params)
            {
                if (p.dir == 0)   // input
                {
                    if (argBuf.size() <= inIdx) argBuf.resize(inIdx + 1);
                    UI::Text("  [in] %s :", p.name.c_str()); UI::SameLine();
                    UI::PushID((int)inIdx); UI::SetNextItemWidth(180.f);
                    UI::InputTextString("##arg", argBuf[inIdx], 0, p.type.c_str());
                    UI::PopID();
                    ++inIdx;
                }
                else
                    UI::TextDisabled("  %s %s : %s", p.dir == 2 ? "[ret]" : "[out]", p.name.c_str(), p.type.c_str());
            }
            //but what if..
            //update: seems to work LOL
            if (/* fv.invokable */ true)
            {
                if (UI::SmallButton("Call"))
                {
                    auto* obj = reinterpret_cast<UObject*>(objAddr);
                    std::vector<std::string> args(argBuf.begin(), argBuf.end());
                    EnqueueOnce([obj, objIdx, func, args]() { InvokeFunctionGameThread(obj, objIdx, func, args); });
                }
                UI::SameLine(); UI::TextDisabled("(dispatched to game thread)");
            }
            else
                UI::TextDisabled("  [not BlueprintCallable / Exec]");

            std::string res;
            { std::lock_guard<std::mutex> lk(s_callResMx); auto it = s_callResults.find(func); if (it != s_callResults.end()) res = it->second; }
            if (!res.empty())
            {
                UI::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.90f, 0.55f, 1.f));
                UI::TextWrapped("\xE2\x86\x92 %s", res.c_str());
                UI::PopStyleColor();
                if (UI::BeginPopupContextItem("##callres"))
                {
                    if (UI::MenuItem("Copy result")) UI::SetClipboardText(res.c_str());
                    if (UI::MenuItem("Clear result")) { std::lock_guard<std::mutex> lk(s_callResMx); s_callResults.erase(func); }
                    UI::EndPopup();
                }
            }
            UI::TreePop();
        }
        UI::PopID();
    }

    // Renders a whole ObjectView. Returns nothing; jumps post via s_treeJump* (consumed by the tab).
    static void RenderObjectView(const ObjView::ObjectView& v, std::set<uint64_t>& expanded,
                                 const char* fieldFilter, bool hideEmptyClasses)
    {
        // Header: name + class-jump + outer-jump
        UI::TextUnformatted(v.objName.c_str());
        UI::SameLine();
        {
            char cls[256]; snprintf(cls, sizeof cls, "[%s]##cls", v.className.c_str());
            if (UI::SmallButton(cls) && v.classAddr) s_treeJumpClassAddr = v.classAddr;
        }
        if (v.outerAddr)
        {
            UI::SameLine(); UI::TextDisabled("outer:"); UI::SameLine();
            char out[256]; snprintf(out, sizeof out, "%s##out", v.outerName.c_str());
            if (UI::SmallButton(out)) s_treeJumpAddr = v.outerAddr;
        }
        UI::Separator();

        for (size_t lvl = 0; lvl < v.chain.size(); ++lvl)
        {
            const auto& clv = v.chain[lvl];
            if (hideEmptyClasses && clv.props.empty() && clv.funcs.empty()) continue;
            UI::PushID((int)lvl);
            UI::PushStyleColor(ImGuiCol_Text, ClassLevelColor(clv.level));
            const ImGuiTreeNodeFlags f = (lvl == 0) ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
            const bool open = UI::TreeNodeEx(clv.className.c_str(), f);
            UI::PopStyleColor();
            if (open)
            {
                bool any = false;
                for (const auto& n : clv.props)
                {
                    if (fieldFilter[0] && !IContains(n.name, fieldFilter)) continue;
                    any = true;
                    RenderModelNode(n, expanded);
                }
                if (!any && fieldFilter[0]) UI::TextDisabled("(no matches)");
                else if (clv.props.empty())  UI::TextDisabled("(no properties)");
                UI::TreePop();
            }
            UI::PopID();
        }

        // Functions section
        bool anyFuncs = false;
        for (const auto& clv : v.chain) if (!clv.funcs.empty()) { anyFuncs = true; break; }
        if (anyFuncs)
        {
            UI::Spacing(); UI::Separator(); UI::TextDisabled("Functions"); UI::Spacing();
            UI::PushID("##mfuncs");
            for (const auto& clv : v.chain)
            {
                if (clv.funcs.empty()) continue;
                UI::PushID(clv.level);
                UI::PushStyleColor(ImGuiCol_Text, ClassLevelColor(clv.level));
                const bool open = UI::TreeNodeEx(clv.className.c_str(), ImGuiTreeNodeFlags_None);
                UI::PopStyleColor();
                if (open) { for (const auto& fv : clv.funcs) RenderModelFunc(v.addr, v.index, fv); UI::TreePop(); }
                UI::PopID();
            }
            UI::PopID();
        }
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

        // Property tree: hide ancestor classes with no reflected properties and no functions
        bool m_hideEmptyClasses = true;

        // Render the right pane from the game-thread ObjectView snapshot (no live UI-thread
        // reads). m_expanded holds the expansion node-keys published to the producer request.
        std::set<uint64_t> m_expanded;

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
                if (newSel) { UI::SetScrollHereY(0.f); m_expanded.clear(); }
                if (m_selectedAddr)
                {
                    UI::SetNextItemWidth(-1.f);
                    UI::InputTextWithHint("##ffilter", "filter fields...",
                                         m_fieldFilter, sizeof(m_fieldFilter));
                    UI::Checkbox("Hide empty classes", &m_hideEmptyClasses);

                    // Publish the request (focus + expanded keys) and beat the snapshot so the
                    // game-thread producer rebuilds the focused object every tick; render the
                    // resulting snapshot WITHOUT touching live memory.
                    ObjView::Request req;
                    req.focusAddr  = m_selectedAddr;
                    req.focusIndex = m_selectedIdx;
                    req.expanded.assign(m_expanded.begin(), m_expanded.end());   // set → sorted
                    detail::SetObjViewRequest(req);
                    detail::ObjectViewSnap().beat(detail::NowMs());
                    const auto view = detail::ObjectViewSnap().read();
                    if (view.valid && view.addr == m_selectedAddr)
                    {
                        m_selectedIdx = view.index;   // adopt game-thread truth (fixes post-jump idx)
                        RenderObjectView(view, m_expanded, m_fieldFilter, m_hideEmptyClasses);
                    }
                    else
                        UI::TextDisabled("(building… or object destroyed)");

                    if (s_treeJumpAddr)      { JumpTo(s_treeJumpAddr);          s_treeJumpAddr = 0; }
                    if (s_treeJumpClassAddr) { JumpToClass(s_treeJumpClassAddr); s_treeJumpClassAddr = 0; }
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
