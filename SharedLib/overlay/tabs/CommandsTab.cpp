// CommandsTab.cpp — overlay "Commands" tab: browse registered commands by
// category, quick-run no-arg commands, or pass arguments to a selected one.
//
// Layer: game (Layer 3). Registered via detail::RegisterCommandsTab().

#include "../OverlayTabs.h"
#include "../Lib_OverlayUI.h"                 // UI::FilterBox / FilterMatch
#include "../../game/Lib_CommandHandler.h"   // CommandHandler::GetEntries / CommandEntry

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

            std::string filter;                // command-name filter
            char        args[512]      = {};   // args for the selected command
            std::string selCmd;                // currently selected command
            std::string selDesc;

            void Draw()
            {
            auto* h = detail::Handler();
            if (!h) { UI::TextDisabled("(no command handler bound)"); return; }

            UI::FilterBox("##filter", filter, -1.f, "filter commands...");

            // Group by category (sorted).
            std::map<std::string, std::vector<std::pair<std::string, std::string>>> byCat;
            for (const auto& [name, entry] : h->GetEntries())
            {
                if (!UI::FilterMatch(filter, name)) continue;
                byCat[entry.category].push_back({ name, entry.description });
            }

            // Reserve exactly what the footer below the list actually uses, so the
            // list gets the rest (over-reserving shrank it; under-reserving pushed the
            // run row out of view). Footer = separator + one text line (hint, or the
            // selected name) + the args input row only when a command is selected.
            const float footerH = UI::GetStyle().ItemSpacing.y * 2.f
                                + UI::GetTextLineHeightWithSpacing()
                                + (selCmd.empty() ? 0.f : UI::GetFrameHeightWithSpacing());

            if (UI::BeginChild("##cmds", ImVec2(0, -footerH)))
            {
                for (auto& [cat, cmds] : byCat)
                {
                    std::sort(cmds.begin(), cmds.end());
                    if (UI::CollapsingHeader(cat.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                        for (auto& [name, desc] : cmds)
                        {
                            UI::PushID(name.c_str());
                            if (UI::SmallButton("Run")) RunCommand(name);   // quick no-arg run
                            UI::SameLine();
                            if (UI::Selectable(name.c_str(), selCmd == name))
                            {
                                selCmd = name;       // select → args footer targets it
                                selDesc = desc;
                                args[0] = '\0';
                            }
                            if (!desc.empty() && UI::IsItemHovered())
                                UI::SetTooltip("%s", desc.c_str());
                            UI::PopID();
                        }
                }
            }
            UI::EndChild();

            // ── Selected-command runner (with arguments) ─────────────────────────
            UI::Separator();
            if (selCmd.empty())
            {
                UI::TextDisabled("Select a command to pass arguments, or use Run for no-arg commands.");
            }
            else
            {
                UI::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.f), "%s", selCmd.c_str());
                if (!selDesc.empty())
                {
                    UI::SameLine();
                    UI::TextDisabled("— %s", selDesc.c_str());
                }
                UI::SetNextItemWidth(-60.f);
                bool enter = UI::InputTextWithHint("##args", "arguments (e.g. 0 0 0)...",
                                                      args, sizeof(args), ImGuiInputTextFlags_EnterReturnsTrue);
                UI::SameLine();
                if (UI::Button("Run##sel") || enter)
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
