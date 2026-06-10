#include "../OverlayTabs.h"
#include "../Lib_OverlayUI.h"      // UI::ComboFromList

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

                if (ImGui::Button("Run action"))
                {
                    const std::string cmd = choices[selected] == "Random" ? "randneg" : "negotiation " + choices[selected];
                    detail::RunCommand(cmd);
                }

                UI::SetNextItemWidth(260.f);
                UI::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(FLT_MAX, UI::GetTextLineHeightWithSpacing() * 15.f));
                UI::ComboFromList("Selection", &selected, choices, /*searchable*/ true);
            }
        };

        struct ResourceTab : detail::OverlayTab<ResourceTab>
        {
            static constexpr const char* kName = "Resources";

            int amount = 0;
            int choice = 0;

            const std::vector<std::string> Choices = { "Add XP", "Add XP Player", "Add Levels" };
            const char* const Functions[3] = { "Cheat_AddXP", "Cheat_AddXP_Player", "Cheat_LevelUp" };

            void Draw()
            {
                UI::SetNextItemWidth( 180.f );
                UI::InputInt("Amount", &amount);
                amount = amount < 0 ? 0 : amount;

                UI::SetNextItemWidth(180.f);
                UI::SetNextWindowSizeConstraints(
                    ImVec2(0.f, 0.f),
                    ImVec2(FLT_MAX, UI::GetTextLineHeightWithSpacing() * 15.f)
                );

                if (UI::Button("Run"))
                {
                    const bool levelup = choice == 2;
                    const std::string cmd = levelup ? "levelup " + std::to_string(amount) : "call " + std::string(Functions[choice]) + " :: " + std::to_string(amount);
                    detail::RunCommand(cmd);
                }

                UI::ComboFromList("Selection", &choice, Choices, /*searchable*/ false, /*programmatic_overrides*/ {}, /*override_fn*/ nullptr, /*allow_rename*/ false);

                
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
