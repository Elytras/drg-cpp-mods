#include "../OverlayTabs.h"

#include <imgui.h>

#include <string>
#include <vector>
#include "../../hooks/Lib_GameHooks.h"
#if defined(RogueCore) && RogueCore

namespace OverlayConsole
{
    namespace
    {
        GameThreadSnapshot<std::vector<std::string>> g_negotiations;

        struct NegotiationTab : detail::OverlayTab<NegotiationTab>
        {
            static constexpr const char* kName = "Negotiations";

            int selected = 0;
            ImGuiTextFilter filter;

            void Draw()
            {
                std::vector<std::string> choices = detail::Negotiations().read();
                if (choices.empty())
                {
                    ImGui::TextDisabled("(no negotiations)");
                    return;
                }

                choices.push_back("Random");

                if (selected >= static_cast<int>(choices.size()))
                    selected = 0;

                ImGui::SetNextItemWidth(260.f);
                ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(FLT_MAX, ImGui::GetTextLineHeightWithSpacing() * 15.f));

                const char* preview =
                    (selected >= 0 && selected < static_cast<int>(choices.size()))
                    ? choices[selected].c_str()
                    : "None";
                
                if (ImGui::Button("Run action"))
                {
                    if (choices[selected] == "Random")
                    {
                        detail::RunCommand("randneg");
                    }
                    else
                    {
                        detail::RunCommand(
                            "call Cheat_StartNegotiation :: fn:localplayer " +
                            choices[selected]
                        );
                    }
                }
                
                if (ImGui::BeginCombo("Selection", preview))
                {
                    filter.Draw("Search", 240.f);

                    ImGui::Separator();

                    for (int i = 0; i < static_cast<int>(choices.size()); ++i)
                    {
                        const std::string& item = choices[i];

                        // Filter check
                        if (!filter.PassFilter(item.c_str()))
                            continue;

                        const bool isSelected = (selected == i);

                        if (ImGui::Selectable(item.c_str(), isSelected))
                            selected = i;

                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }

                    ImGui::EndCombo();
                }

                
            }
        };

        struct ResourceTab : detail::OverlayTab<ResourceTab>
        {
            static constexpr const char* kName = "Resources";

            int amount = 0;
            int choice = 0;

            const char* const Choices[3] = { "Add XP", "Add XP Player", "Add Levels" };
            const char* const Functions[3] = { "Cheat_AddXP", "Cheat_AddXP_Player", "Cheat_LevelUp" };

            void Draw()
            {
                ImGui::SetNextItemWidth( 180.f );
                ImGui::InputInt("Amount", &amount);
                amount = amount < 0 ? 0 : amount;

                ImGui::SetNextItemWidth(180.f);
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(0.f, 0.f),
                    ImVec2(FLT_MAX, ImGui::GetTextLineHeightWithSpacing() * 15.f)
                );

                if (ImGui::BeginCombo("Selection", Choices[choice]))
                {
                    for (int i = 0; i < IM_ARRAYSIZE(Choices); ++i)
                    {
                        const bool isSelected = choice == i;
                        if (ImGui::Selectable(Choices[i], isSelected))
                            choice = i;
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }

                    ImGui::EndCombo();
                }

                if  (ImGui::Button("Run"))
                {
                    const bool levelup = choice == 2;
                    const std::string cmd = levelup ? "levelup " + std::to_string(amount) : "call " + std::string(Functions[choice]) + " :: " + std::to_string(amount);
                    detail::RunCommand(cmd);
                }
            }
        };
    }

    GameThreadSnapshot<std::vector<std::string>>& detail::Negotiations()
    {
        return g_negotiations;
    }

    void detail::RegisterNegotiationTab()
    {
        static NegotiationTab s;
        (void)s;
    }

    void detail::RegisterResourceTab()
    {
        static ResourceTab s;
        (void)s;
    }
}

#endif
