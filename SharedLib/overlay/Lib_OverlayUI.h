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
