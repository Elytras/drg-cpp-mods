#pragma once
// Lib_OverlayUI.h — UI:: = ImGui + the small widgets ImGui lacks that we'd otherwise
// reinvent per-tab.
//
// `using namespace ImGui;` below means a qualified UI::Name resolves to every ImGui name
// (UI::Button, UI::Text, UI::BeginTable, …) AND to our additions — so overlay code uses
// ONE prefix everywhere: UI::. Prefer UI:: over ImGui:: in overlay/tab code; reach for the
// extensions here instead of hand-rolling them.
//
// RULE: only add what ImGui genuinely lacks. Never re-wrap an ImGui call 1:1 — that's what
// `using namespace ImGui;` is for.
//
// Layer: overlay (Layer 3). Header-only for now; promote bodies to a Lib_OverlayUI.cpp if
// any grow large.

#include <imgui.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "StringLib.h"          // StringLib::IContains (FilterMatch — canonical CI substring)
#include "Lib_Overlay.h"        // Overlay key-capture (KeybindButton)
#include "Lib_KeyBindings.h"    // Key / Mod / ChordLabel (KeybindButton label)

namespace UI
{
    using namespace ImGui;

    namespace detail
    {
        // Resize callback for std::string-backed InputText (the imgui_stdlib pattern,
        // vendored here so we don't depend on misc/cpp/imgui_stdlib).
        inline int StringResizeCb(ImGuiInputTextCallbackData* d)
        {
            if (d->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                auto* s = static_cast<std::string*>(d->UserData);
                s->resize((size_t)d->BufTextLen);
                d->Buf = s->data();
            }
            return 0;
        }
    }

    // InputText bound directly to a std::string (auto-grows) — no char buffer + strncpy_s
    // dance. `hint` (optional) shows placeholder text when empty. Returns true on edit per
    // the usual InputText flags (e.g. EnterReturnsTrue).
    inline bool InputTextString(const char* label, std::string& s,
                                ImGuiInputTextFlags flags = 0, const char* hint = nullptr)
    {
        flags |= ImGuiInputTextFlags_CallbackResize;
        return hint
            ? ImGui::InputTextWithHint(label, hint, s.data(), s.capacity() + 1, flags, detail::StringResizeCb, &s)
            : ImGui::InputText(label, s.data(), s.capacity() + 1, flags, detail::StringResizeCb, &s);
    }

    // A filter text box (std::string-backed, with a hint). `width` < 0 stretches to fill.
    // Pair with FilterMatch() for the test, so every tab filters the same (case-insensitive
    // substring via the canonical StringLib::IContains) instead of hand-rolling tolower+find.
    inline bool FilterBox(const char* id, std::string& text, float width = -1.f,
                          const char* hint = "filter…")
    {
        if (width != 0.f) ImGui::SetNextItemWidth(width);
        return InputTextString(id, text, 0, hint);
    }

    // Case-insensitive substring test; an empty filter matches everything.
    inline bool FilterMatch(const std::string& filter, std::string_view text)
    {
        return filter.empty() || StringLib::IContains(text, filter);
    }

    // A combo backed by a vector<string>. Sets *index to the chosen item and returns true
    // when the selection changes. `searchable` adds a per-combo filter box inside the popup
    // (FilterMatch). Replaces the hand-rolled BeginCombo + Selectable loop (+ ImGuiTextFilter).
    inline bool ComboFromList(const char* label, int* index, const std::vector<std::string>& items,
                              bool searchable = false)
    {
        const char* preview = (*index >= 0 && *index < (int)items.size()) ? items[*index].c_str() : "";
        bool changed = false;
        if (ImGui::BeginCombo(label, preview))
        {
            std::string* filter = nullptr;
            if (searchable)
            {
                static std::unordered_map<ImGuiID, std::string> s_filters;   // per-combo, popup-scoped
                filter = &s_filters[ImGui::GetID(label)];
                ImGui::SetNextItemWidth(-FLT_MIN);
                InputTextString("##combosearch", *filter, 0, "search…");
                ImGui::Separator();
            }
            for (int i = 0; i < (int)items.size(); ++i)
            {
                if (filter && !FilterMatch(*filter, items[i])) continue;
                const bool sel = (*index == i);
                if (ImGui::Selectable(items[i].c_str(), sel)) { *index = i; changed = true; }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    // A dimmed "(?)" that shows `text` as a wrapped tooltip on hover — the classic ImGui
    // demo helper, so it lives in one place instead of being re-pasted per tab.
    inline void HelpMarker(const char* text)
    {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // Press-to-capture rebind button. Shows the current key's label; on click it listens
    // (overlay key-capture) for the next key / mouse button — Esc cancels — and when one is
    // captured writes its VK into *vk and returns true (the caller then applies it, e.g.
    // Overlay::SetToggleKey). `id` must be unique per button. Multi-button safe: only the
    // button that started the capture shows "listening" and consumes the result.
    inline bool KeybindButton(const char* id, uint16_t* vk, float width = 200.f)
    {
        static std::string s_active;   // id of the button that started the capture ("" = none)

        const bool listening = Overlay::IsCapturingKey() && s_active == id;
        const std::string label = listening
            ? "press a key…  (Esc cancels)"
            : KeyBindings::ChordLabel((Key)*vk, Mod::None);

        if (listening) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.22f, 0.05f, 1.f));
        if (ImGui::Button((label + "###" + id).c_str(), ImVec2(width, 0.f)) && !Overlay::IsCapturingKey())
        {
            Overlay::BeginKeyCapture();
            s_active = id;
        }
        if (listening) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !listening)
            ImGui::SetTooltip("Click, then press any key or mouse button to rebind");

        // Consume the result if THIS button started the capture. The capture clears the
        // "capturing" flag the instant a key arrives, so gate on ownership (s_active) — NOT
        // on IsCapturingKey(), which is already false by the time we read the result, leaving
        // the captured key forever unconsumed and the rebind a silent no-op.
        if (s_active == id)
        {
            uint16_t captured;
            if (Overlay::TakeCapturedKey(&captured)) { *vk = captured; s_active.clear(); return true; }
            if (!Overlay::IsCapturingKey()) s_active.clear();   // Esc/cancel with no capture
        }
        return false;
    }
}
