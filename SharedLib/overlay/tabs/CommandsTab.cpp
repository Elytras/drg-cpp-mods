// CommandsTab.cpp — overlay "Commands" tab: browse registered commands by
// category, quick-run no-arg commands, or pass arguments to a selected one.
//
// Layer: game (Layer 3). Registered via detail::RegisterCommandsTab().

#include "OverlayTabs.h"
#include "Lib_CommandHandler.h"   // CommandHandler::GetEntries / CommandEntry

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace OverlayConsole
{
    namespace
    {
        using detail::RunCommand;

        struct CommandsTab : detail::OverlayTab<CommandsTab>
        {
            static constexpr const char* kName = "Commands";

            char        filterBuf[128] = {};   // command-name filter
            char        args[512]      = {};   // args for the selected command
            std::string selCmd;                // currently selected command
            std::string selDesc;

            void Draw()
            {
            auto* h = detail::Handler();
            if (!h) { ImGui::TextDisabled("(no command handler bound)"); return; }

            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputTextWithHint("##filter", "filter commands...", filterBuf, sizeof(filterBuf));
            std::string filter = filterBuf;
            std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

            // Group by category (sorted).
            std::map<std::string, std::vector<std::pair<std::string, std::string>>> byCat;
            for (const auto& [name, entry] : h->GetEntries())
            {
                std::string lname = name;
                std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);
                if (!filter.empty() && lname.find(filter) == std::string::npos) continue;
                byCat[entry.category].push_back({ name, entry.description });
            }

            // Reserve exactly what the footer below the list actually uses, so the
            // list gets the rest (over-reserving shrank it; under-reserving pushed the
            // run row out of view). Footer = separator + one text line (hint, or the
            // selected name) + the args input row only when a command is selected.
            const float footerH = ImGui::GetStyle().ItemSpacing.y * 2.f
                                + ImGui::GetTextLineHeightWithSpacing()
                                + (selCmd.empty() ? 0.f : ImGui::GetFrameHeightWithSpacing());

            if (ImGui::BeginChild("##cmds", ImVec2(0, -footerH)))
            {
                for (auto& [cat, cmds] : byCat)
                {
                    std::sort(cmds.begin(), cmds.end());
                    if (ImGui::CollapsingHeader(cat.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                        for (auto& [name, desc] : cmds)
                        {
                            ImGui::PushID(name.c_str());
                            if (ImGui::SmallButton("Run")) RunCommand(name);   // quick no-arg run
                            ImGui::SameLine();
                            if (ImGui::Selectable(name.c_str(), selCmd == name))
                            {
                                selCmd = name;       // select → args footer targets it
                                selDesc = desc;
                                args[0] = '\0';
                            }
                            if (!desc.empty() && ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", desc.c_str());
                            ImGui::PopID();
                        }
                }
            }
            ImGui::EndChild();

            // ── Selected-command runner (with arguments) ─────────────────────────
            ImGui::Separator();
            if (selCmd.empty())
            {
                ImGui::TextDisabled("Select a command to pass arguments, or use Run for no-arg commands.");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.f), "%s", selCmd.c_str());
                if (!selDesc.empty())
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("— %s", selDesc.c_str());
                }
                ImGui::SetNextItemWidth(-60.f);
                bool enter = ImGui::InputTextWithHint("##args", "arguments (e.g. 0 0 0)...",
                                                      args, sizeof(args), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if (ImGui::Button("Run##sel") || enter)
                {
                    std::string line = selCmd;
                    if (args[0]) { line += ' '; line += args; }
                    RunCommand(line);
                }
            }
            }
        };
    } // anonymous namespace

    void detail::RegisterCommandsTab() { static CommandsTab s; (void)s; }
}
