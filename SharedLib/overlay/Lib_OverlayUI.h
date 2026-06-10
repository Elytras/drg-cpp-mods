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

    // Press-to-capture rebind button. Shows the current key's label; on click it listens
    // (overlay key-capture) for the next key / mouse button — Esc cancels — and when one is
    // captured writes its VK into *vk and returns true (the caller then applies it, e.g.
    // Overlay::SetToggleKey). `id` must be unique per button. Multi-button safe: only the
    // button that started the capture shows "listening" and consumes the result.
    inline bool KeybindButton(const char* id, uint16_t* vk, float width = 200.f)
    {
        static std::string s_active;   // id of the button currently capturing ("" = none)

        const bool mine = Overlay::IsCapturingKey() && s_active == id;
        const std::string label = mine
            ? "press a key…  (Esc cancels)"
            : KeyBindings::ChordLabel((Key)*vk, Mod::None);

        if (mine) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.22f, 0.05f, 1.f));
        if (ImGui::Button((label + "###" + id).c_str(), ImVec2(width, 0.f)) && !Overlay::IsCapturingKey())
        {
            Overlay::BeginKeyCapture();
            s_active = id;
        }
        if (mine) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !mine)
            ImGui::SetTooltip("Click, then press any key or mouse button to rebind");

        if (mine)
        {
            uint16_t captured;
            if (Overlay::TakeCapturedKey(&captured)) { *vk = captured; s_active.clear(); return true; }
            if (!Overlay::IsCapturingKey()) s_active.clear();   // Esc/cancel
        }
        return false;
    }
}
