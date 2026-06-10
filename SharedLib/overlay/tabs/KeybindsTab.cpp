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
            {
                static char s_otk[32] = {};
                const std::string cur = KeyBindings::ChordLabel((Key)Overlay::GetToggleKey(), Mod::None);
                ImGui::TextUnformatted("Overlay toggle:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.f);
                const bool ed = ImGui::InputText("##otk", s_otk, sizeof(s_otk),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
                const bool editing = ImGui::IsItemActive();
                ImGui::SameLine();
                const bool set = ImGui::Button("Set##otk");
                if ((set || ed) && s_otk[0])
                {
                    Key k; Mod m;
                    if (KeyBindings::ParseChord(s_otk, k, m)) Overlay::SetToggleKey((uint16_t)k);
                    else info("[overlay] unknown key '{}'", s_otk);
                }
                // Reflect the live binding only when not editing/committing — doing this
                // unconditionally clobbers the typed value on the same frame the Set button
                // steals focus from the InputText (IsItemActive() flips to false), so the
                // commit above would re-parse the *old* key and the rebind would no-op.
                else if (!editing) strncpy_s(s_otk, sizeof(s_otk), cur.c_str(), _TRUNCATE);
            }
            ImGui::Separator();

            ImGui::TextDisabled("All keybinds. User binds (from `bind`) are editable; code binds are read-only.");

            const auto binds = KeyBindings::SnapshotAll();

            const ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_ScrollY;
            // Leave room for the add-row beneath the table.
            const float footH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
            int rowId = 0;
            if (ImGui::BeginTable("##kb", 3, tf, ImVec2(0, -footH)))
            {
                ImGui::TableSetupColumn("key",     ImGuiTableColumnFlags_WidthFixed, 150.f);
                ImGui::TableSetupColumn("binding");
                ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 70.f);
                ImGui::TableHeadersRow();

                if (binds.empty())
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("(none)");
                }

                for (const auto& b : binds)
                {
                    ImGui::TableNextRow();
                    ImGui::PushID(rowId++);   // chords aren't unique across binds

                    ImGui::TableSetColumnIndex(0);
                    if (b.cli)
                    {
                        // Editable chord; Enter rebinds (unbind old + bind new).
                        std::string& s = kbChordEdit[b.chord];
                        char buf[64];
                        strncpy_s(buf, sizeof(buf), s.c_str(), _TRUNCATE);
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::InputText("##chord", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
                            if (buf[0] && b.chord != buf)
                            {
                                RunCommand("unbind " + b.chord);
                                RunCommand(std::string("bind ") + buf + " " + b.command);
                            }
                        s = buf;
                        if (!ImGui::IsItemActive()) s = b.chord;
                    }
                    else
                    {
                        ImGui::TextUnformatted(b.chord.c_str());
                    }

                    // binding column: command for CLI binds; for code binds show the
                    // human label (what it does) + a dim focus/trigger qualifier.
                    ImGui::TableSetColumnIndex(1);
                    if (b.cli)
                        ImGui::TextUnformatted(b.command.c_str());
                    else
                    {
                        if (!b.label.empty()) { ImGui::TextUnformatted(b.label.c_str()); ImGui::SameLine(); }
                        std::string q = "(" + b.focus + " " + b.trigger + (b.suppress ? " suppress" : "") + ")";
                        ImGui::TextDisabled("%s", q.c_str());
                    }

                    ImGui::TableSetColumnIndex(2);
                    if (b.cli) { if (ImGui::SmallButton("Unbind")) RunCommand("unbind " + b.chord); }
                    else         ImGui::TextDisabled("code");

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            // Add-row: chord + command → bind.
            ImGui::SetNextItemWidth(140.f);
            ImGui::InputTextWithHint("##addchord", "key (e.g. F3)", kbAddChord, sizeof(kbAddChord));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-90.f);
            bool addEnter = ImGui::InputTextWithHint("##addcmd", "command...", kbAddCmd, sizeof(kbAddCmd),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("Bind") || addEnter) && kbAddChord[0] && kbAddCmd[0])
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
