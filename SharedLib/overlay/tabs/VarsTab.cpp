// VarsTab.cpp — overlay "Vars" tab: VarSystem viewer/editor. Reads the snapshot
// produced on the game thread; commits edits via `set`/`unset` commands.
//
// Layer: game (Layer 3). Registered via detail::RegisterVarsTab(). The snapshot
// (detail::Vars()) is produced by the tick in OverlayConsole::Init.

#include "OverlayTabs.h"
#include "CoreUtils.h"          // SafeStof / SafeStoll (canonical parsers)

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
            const float footH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
            if (ImGui::BeginTable("##vars", 4, tf, ImVec2(0, -footH)))
            {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 170.f);
                ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 60.f);
                ImGui::TableSetupColumn("value");
                ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, 28.f);
                ImGui::TableHeadersRow();

                if (vars.empty())
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("(none)");
                }

                for (const auto& v : vars)
                {
                    ImGui::TableNextRow();
                    ImGui::PushID(v.name.c_str());
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(v.name.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", VarSystem::TypeName(v.type));
                    ImGui::TableSetColumnIndex(2);
                    ImGui::SetNextItemWidth(-1.f);

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
                        if (ImGui::Checkbox("##b", &b)) { SetVar(v.name, b ? "true" : "false"); pendB[v.name] = b; }
                        break;
                    }
                    case VT::Float:
                    {
                        float& f = editF[v.name];
                        ImGui::DragFloat("##f", &f, 0.1f);
                        if (ImGui::IsItemDeactivatedAfterEdit()) { SetVar(v.name, std::to_string(f)); pendF[v.name] = f; }
                        if (!ImGui::IsItemActive())
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
                        ImGui::DragInt("##i", &i);
                        if (ImGui::IsItemDeactivatedAfterEdit()) { SetVar(v.name, std::to_string(i)); pendI[v.name] = i; }
                        if (!ImGui::IsItemActive())
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
                        char buf[256];
                        strncpy_s(buf, sizeof(buf), s.c_str(), _TRUNCATE);
                        if (ImGui::InputText("##s", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
                        { SetVar(v.name, buf); pendS[v.name] = buf; }
                        s = buf;
                        if (!ImGui::IsItemActive())
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

                    ImGui::TableSetColumnIndex(3);
                    if (ImGui::SmallButton("x")) RunCommand("unset " + v.name);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("unset %s", v.name.c_str());

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            // Add-row: name + value → set.
            ImGui::SetNextItemWidth(160.f);
            ImGui::InputTextWithHint("##addvname", "name", addName, sizeof(addName));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-90.f);
            bool addEnter = ImGui::InputTextWithHint("##addvval", "value", addVal, sizeof(addVal),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("Set##addv") || addEnter) && addName[0] && addVal[0])
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
