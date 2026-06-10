// VarsTab.cpp — overlay "Vars" tab: VarSystem viewer/editor. Reads the snapshot
// produced on the game thread; commits edits via `set`/`unset` commands.
//
// Layer: game (Layer 3). Registered via detail::RegisterVarsTab(). The snapshot
// (detail::Vars()) is produced by the tick in OverlayConsole::Init.

#include "OverlayTabs.h"
#include "CoreUtils.h"          // SafeStof / SafeStoll (canonical parsers)
#include "Lib_OverlayUI.h"      // UI::InputTextString

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace OverlayConsole
{
    namespace
    {
        using detail::RunCommand;
        using detail::VarSnap;

        struct VarsTab : detail::OverlayTab<VarsTab>
        {
            static constexpr const char* kName = "Vars";

            // Persistent per-row edit state so dragging/typing isn't clobbered by the
            // 0.5 s snapshot refresh while a widget is active.
            std::unordered_map<std::string, float>       editF;
            std::unordered_map<std::string, int>         editI;
            std::unordered_map<std::string, std::string> editS;
            // Optimistic "committed, awaiting snapshot" values. `set` dispatches async to
            // the game thread, so the snapshot still holds the OLD value for up to ~0.5 s
            // after a commit. Without this, re-seeding from the snapshot snaps the widget
            // back to the old value (the flicker) until the set propagates. While a pending
            // value differs from the snapshot, we keep showing the committed value; once the
            // snapshot matches, we clear it and resume seeding.
            std::unordered_map<std::string, float>       pendF;
            std::unordered_map<std::string, int>         pendI;
            std::unordered_map<std::string, std::string> pendS;
            std::unordered_map<std::string, bool>        pendB;
            char addName[128] = {};
            char addVal[256]  = {};

            // Commit a var change through the normal command path (game-thread safe).
            static void SetVar(const std::string& name, const std::string& value)
            {
                RunCommand("set " + name + " " + value);
            }

            void Draw()
            {
            std::vector<VarSnap> vars = detail::Vars().read();
            std::sort(vars.begin(), vars.end(),
                      [](const VarSnap& a, const VarSnap& b) { return a.name < b.name; });

            using VT = VarSystem::VarType;
            const ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_ScrollY;
            // Reserve the add-row at the bottom.
            const float footH = UI::GetFrameHeightWithSpacing() + UI::GetStyle().ItemSpacing.y;
            if (UI::BeginTable("##vars", 4, tf, ImVec2(0, -footH)))
            {
                UI::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 170.f);
                UI::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 60.f);
                UI::TableSetupColumn("value");
                UI::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, 28.f);
                UI::TableHeadersRow();

                if (vars.empty())
                {
                    UI::TableNextRow();
                    UI::TableSetColumnIndex(0);
                    UI::TextDisabled("(none)");
                }

                for (const auto& v : vars)
                {
                    UI::TableNextRow();
                    UI::PushID(v.name.c_str());
                    UI::TableSetColumnIndex(0); UI::TextUnformatted(v.name.c_str());
                    UI::TableSetColumnIndex(1); UI::TextDisabled("%s", VarSystem::TypeName(v.type));
                    UI::TableSetColumnIndex(2);
                    UI::SetNextItemWidth(-1.f);

                    // Each typed widget seeds from the snapshot when idle and pushes a
                    // `set` only on edit-commit, so the periodic refresh can't fight it.
                    switch (v.type)
                    {
                    case VT::Bool:
                    {
                        const bool snapB = (v.token == "true" || v.token == "1" || v.token == "True");
                        bool b = snapB;
                        if (auto it = pendB.find(v.name); it != pendB.end())
                        {
                            if (snapB == it->second) pendB.erase(it);   // snapshot caught up
                            else b = it->second;                        // show committed value
                        }
                        if (UI::Checkbox("##b", &b)) { SetVar(v.name, b ? "true" : "false"); pendB[v.name] = b; }
                        break;
                    }
                    case VT::Float:
                    {
                        float& f = editF[v.name];
                        UI::DragFloat("##f", &f, 0.1f);
                        if (UI::IsItemDeactivatedAfterEdit()) { SetVar(v.name, std::to_string(f)); pendF[v.name] = f; }
                        if (!UI::IsItemActive())
                        {
                            const float snap = SafeStof(v.token);
                            if (auto it = pendF.find(v.name); it != pendF.end())
                            {
                                if (NearlyEqual(snap, it->second, 1e-4)) { pendF.erase(it); f = snap; }
                                else f = it->second;
                            }
                            else f = snap;
                        }
                        break;
                    }
                    case VT::Int32:
                    {
                        int& i = editI[v.name];
                        UI::DragInt("##i", &i);
                        if (UI::IsItemDeactivatedAfterEdit()) { SetVar(v.name, std::to_string(i)); pendI[v.name] = i; }
                        if (!UI::IsItemActive())
                        {
                            const int snap = (int)SafeStoll(v.token);
                            if (auto it = pendI.find(v.name); it != pendI.end())
                            {
                                if (snap == it->second) { pendI.erase(it); i = snap; }
                                else i = it->second;
                            }
                            else i = snap;
                        }
                        break;
                    }
                    default:   // String / Vector / Rotator / Name / Object → editable text
                    {
                        std::string& s = editS[v.name];
                        if (UI::InputTextString("##s", s, ImGuiInputTextFlags_EnterReturnsTrue))
                        { SetVar(v.name, s); pendS[v.name] = s; }
                        if (!UI::IsItemActive())
                        {
                            if (auto it = pendS.find(v.name); it != pendS.end())
                            {
                                if (v.token == it->second) { pendS.erase(it); s = v.token; }
                                else s = it->second;
                            }
                            else s = v.token;
                        }
                        break;
                    }
                    }

                    UI::TableSetColumnIndex(3);
                    if (UI::DangerSmallButton("x")) RunCommand("unset " + v.name);
                    if (UI::IsItemHovered()) UI::SetTooltip("unset %s", v.name.c_str());

                    UI::PopID();
                }
                UI::EndTable();
            }

            // Add-row: name + value → set.
            UI::SetNextItemWidth(160.f);
            UI::InputTextWithHint("##addvname", "name", addName, sizeof(addName));
            UI::SameLine();
            UI::SetNextItemWidth(-90.f);
            bool addEnter = UI::InputTextWithHint("##addvval", "value", addVal, sizeof(addVal),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
            UI::SameLine();
            if ((UI::Button("Set##addv") || addEnter) && addName[0] && addVal[0])
            {
                RunCommand(std::string("set ") + addName + " " + addVal);
                addName[0] = '\0';
                addVal[0]  = '\0';
            }
            }
        };
    } // anonymous namespace

    void detail::RegisterVarsTab() { static VarsTab s; (void)s; }
}
