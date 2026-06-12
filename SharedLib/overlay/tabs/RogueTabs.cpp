#include "../OverlayTabs.h"
#include "../Lib_OverlayUI.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "../../hooks/Lib_GameHooks.h"
#include "../../core/StringLib.h"
#if defined(RogueCore) && RogueCore

namespace OverlayConsole
{
    namespace
    {
        GameThreadSnapshot<std::vector<std::string>> g_negotiations;

        struct NegotiationTab : detail::OverlayTab<NegotiationTab>
        {
            static constexpr const char* kName = "Negotiations";

            enum class SearchMode : uint8_t { Filter, Highlight };

            std::string  selected;
            std::string  lastSelected;   // tracks selection across frames for scroll-once
            SearchMode   searchMode = SearchMode::Filter;

            std::map<std::string, std::vector<std::string>> groups;
            std::unordered_map<std::string, std::string>    filters;
            std::vector<std::string>                         lastChoices;

            void RebuildGroups(const std::vector<std::string>& choices)
            {
                groups.clear();
                for (const auto& name : choices)
                {
                    auto sep = name.find('_');
                    std::string prefix = (sep != std::string::npos) ? name.substr(0, sep) : name;
                    groups[prefix].push_back(name);
                }
            }

            // Strip "PREFIX_" leader — column header already carries the prefix.
            static std::string_view DisplayName(const std::string& name, const std::string& prefix)
            {
                const std::size_t skip = prefix.size() + 1;
                if (name.size() > skip && name[prefix.size()] == '_')
                    return std::string_view(name).substr(skip);
                return name;
            }

            // Case-insensitive position of needle in haystack, npos if not found.
            static size_t IFindFirst(std::string_view haystack, std::string_view needle)
            {
                if (needle.empty()) return std::string_view::npos;
                auto it = std::search(haystack.begin(), haystack.end(),
                                      needle.begin(),  needle.end(),
                                      [](unsigned char a, unsigned char b)
                                      { return std::tolower(a) == std::tolower(b); });
                return it == haystack.end() ? std::string_view::npos : size_t(it - haystack.begin());
            }

            // Overlay a translucent amber rect behind the matching substring of
            // the last rendered Selectable. Call right after the Selectable.
            static void DrawMatchHighlight(std::string_view disp, std::string_view filter)
            {
                const size_t pos = IFindFirst(disp, filter);
                if (pos == std::string_view::npos) return;

                const ImVec2 itemMin = UI::GetItemRectMin();
                const float  padX    = UI::GetStyle().FramePadding.x;
                const float  xBefore = UI::CalcTextSize(disp.data(), disp.data() + pos).x;
                const float  xMatch  = UI::CalcTextSize(disp.data() + pos,
                                                         disp.data() + pos + filter.size()).x;
                const ImVec2 hlMin(itemMin.x + padX + xBefore,  itemMin.y + 1.f);
                const ImVec2 hlMax(hlMin.x  + xMatch,
                                   itemMin.y + UI::GetTextLineHeightWithSpacing() - 1.f);
                UI::GetWindowDrawList()->AddRectFilled(hlMin, hlMax, IM_COL32(255, 200, 50, 85));
            }

            void Draw()
            {
                const auto choices = detail::Negotiations().read();
                if (choices.empty()) { UI::TextDisabled("(no negotiations loaded)"); return; }

                if (choices != lastChoices)
                {
                    RebuildGroups(choices);
                    lastChoices = choices;
                    if (!selected.empty() &&
                        !std::count(choices.begin(), choices.end(), selected))
                        selected.clear();
                }

                // ── Action bar ───────────────────────────────────────────────────
                const bool hasSel = !selected.empty();
                if (!hasSel) UI::BeginDisabled();
                if (UI::Button("Run Action") && hasSel)
                    detail::RunCommand("negotiation " + selected);
                if (!hasSel) UI::EndDisabled();

                UI::SameLine();
                if (UI::Button("Random")) detail::RunCommand("randneg");

                UI::SameLine(0.f, 20.f);
                UI::RadioButton("Filter", reinterpret_cast<int*>(&searchMode), 0);
                UI::SameLine();
                UI::RadioButton("Highlight", reinterpret_cast<int*>(&searchMode), 1);

                if (hasSel) { UI::SameLine(0.f, 16.f); UI::TextDisabled("%s", selected.c_str()); }

                UI::Spacing();

                // ── Prefix columns ───────────────────────────────────────────────
                const int  nGroups = (int)groups.size();
                if (nGroups == 0) return;

                constexpr int   kMaxRows = 12;
                constexpr float kColW    = 210.f;
                const     float kLineH   = UI::GetTextLineHeightWithSpacing();
                const     float kFilterH = UI::GetFrameHeightWithSpacing();
                const bool      highlight = (searchMode == SearchMode::Highlight);

                const bool selChanged = (selected != lastSelected);
                lastSelected = selected;

                // Rows a list may show before it scrolls internally. Derived from the space
                // left in the panel (minus the table's header + filter rows and the bottom
                // horizontal scrollbar) so the whole table fits — the parent window never
                // grows its own scrollbar (which used to scroll the headers out of view).
                const float cellPadY = UI::GetStyle().CellPadding.y;
                const float headerH  = kLineH   + cellPadY * 2.f;
                const float filterH  = kFilterH + cellPadY * 2.f;
                const float scrollX  = UI::GetStyle().ScrollbarSize;
                const float availY   = UI::GetContentRegionAvail().y
                                       - headerH - filterH - scrollX - 6.f;
                const int   rowCap   = std::clamp((int)std::floor(availY / kLineH), 1, kMaxRows);

                const ImGuiTableFlags tf =
                    ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                    ImGuiTableFlags_ScrollX      | ImGuiTableFlags_SizingFixedFit;

                // Height 0 → the table auto-fits its own content. Each list child is capped
                // to rowCap (which fits availY above), so the content cannot overflow the
                // panel and no outer scrollbar appears. (A too-small *explicit* height is
                // ignored without ScrollY — the table draws at natural height and overflows.)
                if (!UI::BeginTable("##neg", nGroups, tf, ImVec2(-FLT_MIN, 0.f)))
                    return;

                for (const auto& [prefix, _] : groups)
                    UI::TableSetupColumn(prefix.c_str(), ImGuiTableColumnFlags_WidthFixed, kColW);
                UI::TableHeadersRow();

                // Filter row — pre-inserts filter keys, builds each column's matched set
                // (reused by the list row), and hosts a per-column "random in this prefix"
                // button next to the filter box.
                static std::mt19937 s_rng{ std::random_device{}() };
                std::unordered_map<std::string, std::vector<const std::string*>> matched;

                UI::TableNextRow();
                int col = 0;
                for (auto& [prefix, items] : groups)
                {
                    UI::TableSetColumnIndex(col++);

                    const float btnW = UI::GetFrameHeight();
                    UI::SetNextItemWidth(-(btnW + UI::GetStyle().ItemSpacing.x));
                    UI::InputTextString(("##f" + prefix).c_str(), filters[prefix], 0, "filter\xe2\x80\xa6");

                    const std::string& f  = filters[prefix];
                    auto&              mv = matched[prefix];
                    for (const auto& name : items)
                    {
                        auto disp = DisplayName(name, prefix);
                        if (f.empty() || StringLib::IContains(disp, f) || StringLib::IContains(name, f))
                            mv.push_back(&name);
                    }

                    UI::SameLine(0.f, UI::GetStyle().ItemSpacing.x);
                    UI::BeginDisabled(mv.empty());
                    if (UI::Button(("R##r" + prefix).c_str(), ImVec2(btnW, 0.f)) && !mv.empty())
                    {
                        const std::string& pick =
                            *mv[std::uniform_int_distribution<size_t>(0, mv.size() - 1)(s_rng)];
                        selected = pick;
                        detail::RunCommand("negotiation " + pick);
                    }
                    UI::EndDisabled();
                    if (UI::IsItemHovered() && !mv.empty())
                        UI::SetTooltip("Random from %s", prefix.c_str());
                }

                // List row — adaptive height per column (visible items, capped).
                UI::TableNextRow();
                col = 0;
                for (const auto& [prefix, items] : groups)
                {
                    UI::TableSetColumnIndex(col);
                    UI::PushID(col++);

                    const std::string& f   = filters[prefix];  // present (filter row above)
                    // Highlight mode shows every item (dimmed if unmatched); Filter mode shows
                    // only the matched set built in the filter row.
                    const int          vis = highlight ? (int)items.size()
                                                       : (int)matched[prefix].size();
                    // Floor at 1 row: BeginChild treats a 0 height as "fill remaining space",
                    // which would blow an empty filtered column up to full panel height.
                    const float        childH = std::min(std::max(vis, 1), rowCap) * kLineH;

                    UI::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
                    const bool childOpen = UI::BeginChild("##list", ImVec2(0, childH), ImGuiChildFlags_None);
                    UI::PopStyleVar();
                    if (childOpen)
                    {
                        for (const auto& name : items)
                        {
                            auto       disp    = DisplayName(name, prefix);
                            const bool matches = f.empty() ||
                                                 StringLib::IContains(disp, f) ||
                                                 StringLib::IContains(name, f);

                            if (!highlight && !matches) continue;

                            const bool isSel = (name == selected);

                            // Dim non-matching entries in Highlight mode.
                            if (highlight && !matches)
                                UI::PushStyleColor(ImGuiCol_Text,
                                                   UI::GetStyleColorVec4(ImGuiCol_TextDisabled));

                            const std::string dispStr(disp);
                            if (UI::Selectable(dispStr.c_str(), isSel))
                                selected = isSel ? "" : name;

                            if (highlight && !matches) UI::PopStyleColor();

                            if (isSel && selChanged) UI::SetScrollHereY(0.5f);

                            // Amber rect over the matched substring.
                            if (highlight && matches && !f.empty())
                                DrawMatchHighlight(disp, f);

                            if (UI::IsItemHovered()) UI::SetTooltip("%s", name.c_str());
                        }
                    }
                    UI::EndChild();
                    UI::PopID();
                }

                UI::EndTable();
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
