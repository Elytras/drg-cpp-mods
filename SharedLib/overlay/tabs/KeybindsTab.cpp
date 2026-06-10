// KeybindsTab.cpp — overlay "Keybinds" tab: list/rebind/unbind user binds, the
// add-row, and the overlay toggle key. Code binds are shown read-only.
//
// Layer: game (Layer 3). Registered via detail::RegisterKeybindsTab().

#include "../OverlayTabs.h"

#if defined(RogueCore) && RogueCore
#include "../../../RcMods/Lib_Forward.h"        
#else 
#include "../../../DrgMods/Lib_Forward.h"
#endif

#include "../../game/Lib_KeyBindings.h"     // KeyBindings, Key, Mod
#include "../Lib_Overlay.h"                 // Overlay::Get/SetToggleKey
#include "../Lib_OverlayUI.h"               // UI:: (ImGui + UI::KeybindButton)

#include <imgui.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace OverlayConsole
{
    namespace
    {
        using detail::RunCommand;

        struct KeybindsTab : detail::OverlayTab<KeybindsTab>
        {
            static constexpr const char* kName = "Keybinds";

            // Per-row chord edit buffer (idle-seeded), plus the add-row.
            std::unordered_map<std::string, std::string> kbChordEdit;
            char kbAddChord[64] = {};
            char kbAddCmd[256]  = {};

            void Draw()
            {
            // Overlay toggle key — owned by Overlay; setting it re-registers the global
            // show-binding and updates the overlay window's close key (single source).
            // Press-to-capture via UI::KeybindButton: click, then press any key / mouse
            // button (Esc cancels). No typing.
            {
                UI::FieldRow("Overlay toggle:", 0.f, /*fillControl*/ false);
                uint16_t k = Overlay::GetToggleKey();
                if (UI::KeybindButton("otk", &k)) Overlay::SetToggleKey(k);
            }
            UI::SectionHeader("Bindings");
            UI::TextDisabled("User binds (from `bind`) are editable; code binds are read-only.");

            const auto binds = KeyBindings::SnapshotAll();

            const ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_ScrollY;
            // Leave room for the add-row beneath the table.
            const float footH = UI::GetFrameHeightWithSpacing() + UI::GetStyle().ItemSpacing.y;
            int rowId = 0;
            if (UI::BeginTable("##kb", 3, tf, ImVec2(0, -footH)))
            {
                UI::TableSetupColumn("key",     ImGuiTableColumnFlags_WidthFixed, 150.f);
                UI::TableSetupColumn("binding");
                UI::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 70.f);
                UI::TableHeadersRow();

                if (binds.empty())
                {
                    UI::TableNextRow();
                    UI::TableSetColumnIndex(0);
                    UI::TextDisabled("(none)");
                }

                for (const auto& b : binds)
                {
                    UI::TableNextRow();
                    UI::PushID(rowId++);   // chords aren't unique across binds

                    UI::TableSetColumnIndex(0);
                    if (b.cli)
                    {
                        // Editable chord; Enter rebinds (unbind old + bind new).
                        std::string& s = kbChordEdit[b.chord];
                        UI::SetNextItemWidth(-1.f);
                        if (UI::InputTextString("##chord", s, ImGuiInputTextFlags_EnterReturnsTrue))
                            if (!s.empty() && b.chord != s)
                            {
                                RunCommand("unbind " + b.chord);
                                RunCommand("bind " + s + " " + b.command);
                            }
                        if (!UI::IsItemActive()) s = b.chord;
                    }
                    else
                    {
                        UI::TextUnformatted(b.chord.c_str());
                    }

                    // binding column: command for CLI binds; for code binds show the
                    // human label (what it does) + a dim focus/trigger qualifier.
                    UI::TableSetColumnIndex(1);
                    if (b.cli)
                        UI::TextUnformatted(b.command.c_str());
                    else
                    {
                        if (!b.label.empty()) { UI::TextUnformatted(b.label.c_str()); UI::SameLine(); }
                        std::string q = "(" + b.focus + " " + b.trigger + (b.suppress ? " suppress" : "") + ")";
                        UI::TextDisabled("%s", q.c_str());
                    }

                    UI::TableSetColumnIndex(2);
                    if (b.cli) { if (UI::DangerSmallButton("Unbind")) RunCommand("unbind " + b.chord); }
                    else         UI::TextDisabled("code");

                    UI::PopID();
                }
                UI::EndTable();
            }

            // Add-row: chord + command → bind.
            UI::SetNextItemWidth(140.f);
            UI::InputTextWithHint("##addchord", "key (e.g. F3)", kbAddChord, sizeof(kbAddChord));
            UI::SameLine();
            UI::SetNextItemWidth(-90.f);
            bool addEnter = UI::InputTextWithHint("##addcmd", "command...", kbAddCmd, sizeof(kbAddCmd),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
            UI::SameLine();
            if ((UI::Button("Bind") || addEnter) && kbAddChord[0] && kbAddCmd[0])
            {
                RunCommand(std::string("bind ") + kbAddChord + " " + kbAddCmd);
                kbAddChord[0] = '\0';
                kbAddCmd[0]   = '\0';
            }
            }
        };
    } // anonymous namespace

    void detail::RegisterKeybindsTab() { static KeybindsTab s; (void)s; }
}
