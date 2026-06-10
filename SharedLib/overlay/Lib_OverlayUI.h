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

#include <functional>
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

    inline std::unordered_map<std::string, std::string>& GetUiOverrides()
    {
        static std::unordered_map<std::string, std::string> s_ui_overrides;
        return s_ui_overrides;
    }

    inline std::string GetComboItemDisplayName(
        const char* combo_label,
        const std::string& item,
        const std::unordered_map<std::string, std::string>& programmatic_overrides = {},
        const std::function<std::string(const std::string&)>& override_fn = nullptr)
    {
        // 1. Programmatic overrides map
        auto it = programmatic_overrides.find(item);
        if (it != programmatic_overrides.end())
            return it->second;

        // 2. Programmatic override function/lambda
        if (override_fn)
        {
            std::string res = override_fn(item);
            if (!res.empty())
                return res;
        }

        // 3. UI-edited overrides
        std::string key = std::string(combo_label) + "##" + item;
        auto& ui_map = GetUiOverrides();
        auto it_ui = ui_map.find(key);
        if (it_ui != ui_map.end())
            return it_ui->second;

        // 4. Default: original item
        return item;
    }

    namespace detail
    {
        // RAII red styling for destructive buttons (unset / unbind / destroy).
        struct DangerColors
        {
            DangerColors()
            {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.15f, 0.15f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.20f, 0.20f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.80f, 0.25f, 0.25f, 1.f));
            }
            ~DangerColors() { ImGui::PopStyleColor(3); }
        };
    }

    // Red-styled buttons for destructive actions, so "delete"-class controls look the same
    // everywhere instead of each tab re-pushing colors (or shipping a plain button).
    inline bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0))
    {
        detail::DangerColors c;
        return ImGui::Button(label, size);
    }
    inline bool DangerSmallButton(const char* label)
    {
        detail::DangerColors c;
        return ImGui::SmallButton(label);
    }

    // A titled section divider with a bit of leading space — groups a panel into labeled
    // sections more clearly than a bare Separator, and consistently across tabs. (Distinct
    // from CollapsingHeader: this is a non-collapsing visual break.)
    inline void SectionHeader(const char* label)
    {
        ImGui::Spacing();
        ImGui::SeparatorText(label);
    }

    // Lay out a labeled control on one line: frame-aligns `label`, then SameLine so the
    // following widget sits next to it. `labelW` > 0 fixes the control's start column (a
    // poor-man's two-column form across rows); `fillControl` sizes the next item to the
    // row's remaining width (leave false for widgets that size themselves, e.g. buttons).
    inline void FieldRow(const char* label, float labelW = 0.f, bool fillControl = true)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
        if (fillControl) ImGui::SetNextItemWidth(-FLT_MIN);
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

    // A combo backed by a vector<string>. Sets *index to the chosen item and returns true
    // when the selection changes. `searchable` adds a per-combo filter box inside the popup
    // (FilterMatch). Replaces the hand-rolled BeginCombo + Selectable loop (+ ImGuiTextFilter).
    // Supports visual overrides both programmatically and via inline right-click menu.
    inline bool ComboFromList(
        const char* label,
        int* index,
        const std::vector<std::string>& items,
        bool searchable = false,
        const std::unordered_map<std::string, std::string>& programmatic_overrides = {},
        std::function<std::string(const std::string&)> override_fn = nullptr,
        bool allow_rename = true)
    {
        std::string preview = "";
        if (*index >= 0 && *index < (int)items.size())
        {
            preview = GetComboItemDisplayName(label, items[*index], programmatic_overrides, override_fn);
        }

        bool changed = false;
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            std::string* filter = nullptr;
            if (searchable)
            {
                static std::unordered_map<ImGuiID, std::string> s_filters;   // per-combo, popup-scoped
                filter = &s_filters[ImGui::GetID(label)];
                if (allow_rename)
                {
                    ImGui::SetNextItemWidth(std::max(150.f, ImGui::GetWindowWidth() - 60.f));
                    InputTextString("##combosearch", *filter, 0, "search…");
                    ImGui::SameLine();
                    HelpMarker("Right-click any entry below to rename/override display name.");
                }
                else
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    InputTextString("##combosearch", *filter, 0, "search…");
                }
                ImGui::Separator();
            }
            else if (allow_rename)
            {
                HelpMarker("Right-click any entry below to rename/override display name.");
                ImGui::Separator();
            }

            // Count matching items to dynamically adjust child height
            int matching_count = 0;
            for (int i = 0; i < (int)items.size(); ++i)
            {
                const std::string disp_name = GetComboItemDisplayName(label, items[i], programmatic_overrides, override_fn);
                if (!filter || FilterMatch(*filter, items[i]) || FilterMatch(*filter, disp_name))
                {
                    matching_count++;
                }
            }

            // Start child scrollable area so searchbar does not scroll away
            float child_height = ImGui::GetTextLineHeightWithSpacing() * std::max(1, std::min(matching_count, 12)) + 4.f;
            if (ImGui::BeginChild("##scrollable_items", ImVec2(0, child_height), ImGuiChildFlags_None, ImGuiWindowFlags_None))
            {
                if (matching_count == 0)
                {
                    ImGui::TextDisabled("No matches");
                }
                else
                {
                    for (int i = 0; i < (int)items.size(); ++i)
                    {
                        const std::string disp_name = GetComboItemDisplayName(label, items[i], programmatic_overrides, override_fn);
                        if (filter && !FilterMatch(*filter, items[i]) && !FilterMatch(*filter, disp_name)) continue;

                        const bool sel = (*index == i);
                        if (ImGui::Selectable(disp_name.c_str(), sel)) { *index = i; changed = true; }
                        if (sel) ImGui::SetItemDefaultFocus();

                        // Unique popup ID for the rename context menu (only if renaming allowed)
                        if (allow_rename)
                        {
                            std::string popup_id = "rename_popup##" + std::string(label) + "##" + items[i];
                            if (ImGui::BeginPopupContextItem(popup_id.c_str()))
                            {
                                static char rename_buf[128] = "";
                                if (ImGui::IsWindowAppearing())
                                {
                                    strncpy_s(rename_buf, disp_name.c_str(), sizeof(rename_buf) - 1);
                                }
                                ImGui::Text("Rename '%s'", items[i].c_str());
                                ImGui::SetNextItemWidth(150.f);
                                ImGui::InputText("##rename_input", rename_buf, sizeof(rename_buf));
                                
                                if (ImGui::Button("Save"))
                                {
                                    std::string new_name(rename_buf);
                                    if (!new_name.empty())
                                    {
                                        std::string key = std::string(label) + "##" + items[i];
                                        GetUiOverrides()[key] = new_name;
                                    }
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Reset"))
                                {
                                    std::string key = std::string(label) + "##" + items[i];
                                    GetUiOverrides().erase(key);
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                            }
                        }
                    }
                }
            }
            ImGui::EndChild();

            ImGui::EndCombo();
        }
        return changed;
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
