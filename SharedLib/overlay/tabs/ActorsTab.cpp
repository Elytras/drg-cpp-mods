// ActorsTab.cpp — overlay "Actors" tab: live GObjects snapshot, filterable/
// sortable table with jump-to-by-addr.
//
// Layer: game (Layer 3). Registered via detail::RegisterActorsTab(). Reads the
// game-thread-built actor snapshot through detail::Actors(); the producing tick
// lives in OverlayConsole::Init.

#ifndef NOMINMAX
#define NOMINMAX               // StringLib pulls <Windows.h>; keep std::min/max usable
#endif
#include "../OverlayTabs.h"
#include "../Lib_OverlayUI.h"
#include "../../core/StringLib.h"         // IEquals / IContains (canonical CI helpers)

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace OverlayConsole
{
    namespace
    {
        using detail::NowMs;
        using StringLib::IEquals;
        using StringLib::IContains;

        struct ActorsTab : detail::OverlayTab<ActorsTab>
        {
            static constexpr const char* kName = "Actors";

            char  fClass[128] = {}, fName[128] = {}, fOuter[128] = {}, fOwner[128] = {}, fInst[128] = {};
            int   classMode = 0;          // 0 Contains, 1 Is, 2 Is+Sub, 3 Not, 4 NotSub
            int   repMode   = 0;          // 0 All, 1 Replicated, 2 Not replicated
            uint64_t actorGotoAddr = 0;   // pending jump-to actor (by unique addr)
            uint64_t actorSelAddr  = 0;   // highlighted row (by unique addr)

            // Does row r pass the class-filter (mode + target text)?
            bool ClassFilterPass(const ActorList::Row& r)
            {
                const std::string t = fClass;
                if (t.empty()) return true;
                const std::string token = "/" + t + "/";   // for subclass chain match
                switch (classMode)
                {
                case 0: return IContains(r.className, t);                 // Contains
                case 1: return IEquals(r.className, t);                 // Is (exact)
                case 2: return IContains(r.classChain, token);           // Is or subclass
                case 3: return !IEquals(r.className, t);                // Not (exact)
                case 4: return !IContains(r.classChain, token);          // Not class or subclasses
                }
                return true;
            }

            void Draw()
            {
            auto& g_actors = detail::Actors();   // game-thread snapshot (produced in Init)
            g_actors.beat(NowMs());   // heartbeat: auto-refresh only runs while this is live

            // ── Controls + per-field filters ─────────────────────────────────────
            if (UI::Button("Refresh")) g_actors.request();
            UI::SameLine();
            bool autoOn = g_actors.isAuto();
            if (UI::Checkbox("Auto", &autoOn)) g_actors.setAuto(autoOn);
            UI::SameLine();
            UI::SetNextItemWidth(120.f);
            UI::Combo("##rep", &repMode, "net: all\0replicated\0not replicated\0");

            UI::SameLine();
            UI::TextDisabled("(right-click a header to show/hide columns)");

            // Per-field filters (one input each).
            UI::SetNextItemWidth(90.f);
            UI::Combo("##cmode", &classMode, "contains\0is\0is+sub\0not\0not+sub\0");
            UI::SameLine(); UI::SetNextItemWidth(150.f);
            UI::InputTextWithHint("##fclass", "class", fClass, sizeof(fClass));
            UI::SameLine(); UI::SetNextItemWidth(130.f);
            UI::InputTextWithHint("##fname", "name", fName, sizeof(fName));
            UI::SameLine(); UI::SetNextItemWidth(120.f);
            UI::InputTextWithHint("##fouter", "outer", fOuter, sizeof(fOuter));
            UI::SameLine(); UI::SetNextItemWidth(120.f);
            UI::InputTextWithHint("##fowner", "owner", fOwner, sizeof(fOwner));
            UI::SameLine(); UI::SetNextItemWidth(-1.f);
            UI::InputTextWithHint("##finst", "instigator", fInst, sizeof(fInst));

            std::vector<ActorList::Row> rows = g_actors.read();
            if (rows.empty()) g_actors.request();   // populate on first view

            const std::string fNameF = fName, fOuterF = fOuter, fOwnerF = fOwner, fInstF = fInst;

            size_t shown = 0;
            const ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
                                     | ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable
                                     | ImGuiTableFlags_Hideable;   // right-click header → show/hide (persists via ini)
            const float actFooter = UI::GetTextLineHeightWithSpacing() + UI::GetStyle().ItemSpacing.y;
            if (UI::BeginTable("##actors", 7, tf, ImVec2(0, -actFooter)))
            {
                UI::TableSetupColumn("class",      ImGuiTableColumnFlags_WidthStretch, 0, 0);
                UI::TableSetupColumn("name",       ImGuiTableColumnFlags_WidthStretch, 0, 1);
                UI::TableSetupColumn("net",        ImGuiTableColumnFlags_WidthFixed,  40, 2);
                UI::TableSetupColumn("outer",      ImGuiTableColumnFlags_WidthStretch, 0, 5);
                UI::TableSetupColumn("owner",      ImGuiTableColumnFlags_WidthStretch, 0, 3);
                UI::TableSetupColumn("instigator", ImGuiTableColumnFlags_WidthStretch, 0, 4);
                // addr = unique instance id; off by default but available (hideable).
                UI::TableSetupColumn("addr",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 120, 6);
                UI::TableSetupScrollFreeze(0, 1);
                UI::TableHeadersRow();

                // Build the filtered index list (uniform clipper needs a flat list).
                static std::vector<int> idx;
                idx.clear();
                for (int i = 0; i < (int)rows.size(); ++i)
                {
                    const auto& r = rows[i];
                    if (repMode == 1 && !r.replicated) continue;
                    if (repMode == 2 &&  r.replicated) continue;
                    if (!ClassFilterPass(r))                            continue;
                    if (!fNameF.empty()  && !IContains(r.name,  fNameF)) continue;
                    if (!fOuterF.empty() && !IContains(r.outer, fOuterF))continue;
                    if (!fOwnerF.empty() && !IContains(r.owner, fOwnerF))continue;
                    if (!fInstF.empty()  && !IContains(r.instigator, fInstF)) continue;
                    idx.push_back(i);
                }
                shown = idx.size();

                // Sort per the active column header.
                if (ImGuiTableSortSpecs* ss = UI::TableGetSortSpecs())
                    if (ss->SpecsCount > 0)
                    {
                        const ImGuiTableColumnSortSpecs& sp = ss->Specs[0];
                        const bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
                        std::sort(idx.begin(), idx.end(), [&](int a, int b)
                        {
                            const auto& ra = rows[a]; const auto& rb = rows[b];
                            int c = 0;
                            switch (sp.ColumnUserID)
                            {
                            case 0: c = ra.className.compare(rb.className); break;
                            case 1: c = ra.name.compare(rb.name);           break;
                            case 2: c = (int)ra.replicated - (int)rb.replicated; break;
                            case 3: c = ra.owner.compare(rb.owner);         break;
                            case 4: c = ra.instigator.compare(rb.instigator); break;
                            case 5: c = ra.outer.compare(rb.outer);         break;
                            case 6: c = (ra.addr < rb.addr) ? -1 : (ra.addr > rb.addr ? 1 : 0); break;
                            }
                            return asc ? c < 0 : c > 0;
                        });
                    }

                // Pending jump-to (outer/owner/instigator click): find its row by the
                // unique addr, force it visible past the clipper, scroll + select it.
                int gotoPos = -1;
                if (actorGotoAddr)
                    for (int k = 0; k < (int)idx.size(); ++k)
                        if (rows[idx[k]].addr == actorGotoAddr) { gotoPos = k; break; }

                // Jump targets a specific instance by addr; clear filters so it's
                // reachable next frame regardless of the current view.
                auto jumpTo = [this](uint64_t targetAddr)
                {
                    if (!targetAddr) return;
                    actorGotoAddr = targetAddr; actorSelAddr = targetAddr;
                    fName[0] = fOuter[0] = fOwner[0] = fInst[0] = fClass[0] = '\0';
                    repMode = 0;
                };

                ImGuiListClipper clipper;
                clipper.Begin((int)idx.size());
                if (gotoPos >= 0) clipper.IncludeItemByIndex(gotoPos);
                while (clipper.Step())
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                    {
                        const auto& r = rows[idx[row]];
                        UI::TableNextRow();
                        UI::PushID(idx[row]);

                        const bool sel = (r.addr && r.addr == actorSelAddr);
                        if (sel) UI::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 90, 130, 120));

                        UI::TableSetColumnIndex(0); UI::TextUnformatted(r.className.c_str());

                        UI::TableSetColumnIndex(1);
                        if (UI::Selectable(r.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
                            actorSelAddr = r.addr;

                        UI::TableSetColumnIndex(2);
                        if (r.replicated) UI::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.f), "rep");
                        else              UI::TextDisabled("-");

                        UI::TableSetColumnIndex(3);
                        if (r.outerAddr)      { if (UI::SmallButton(r.outer.c_str()))      jumpTo(r.outerAddr); }
                        else                    UI::TextDisabled("-");
                        UI::TableSetColumnIndex(4);
                        if (r.ownerAddr)      { if (UI::SmallButton(r.owner.c_str()))      jumpTo(r.ownerAddr); }
                        else                    UI::TextDisabled("-");
                        UI::TableSetColumnIndex(5);
                        if (r.instigatorAddr) { if (UI::SmallButton(r.instigator.c_str())) jumpTo(r.instigatorAddr); }
                        else                    UI::TextDisabled("-");

                        UI::TableSetColumnIndex(6);
                        UI::Text("%llX", (unsigned long long)r.addr);

                        if (gotoPos == row) UI::SetScrollHereY(0.5f);
                        UI::PopID();
                    }
                if (gotoPos >= 0) actorGotoAddr = 0;   // resolved; a fresh click stays pending
                UI::EndTable();
            }
            UI::TextDisabled("%zu shown / %zu actors", shown, rows.size());
            }
        };
    } // anonymous namespace

    void detail::RegisterActorsTab() { static ActorsTab s; (void)s; }
}
