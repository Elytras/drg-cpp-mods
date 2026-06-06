// ActorsTab.cpp — overlay "Actors" tab: live GObjects snapshot, filterable/
// sortable table with jump-to-by-addr.
//
// Layer: game (Layer 3). Registered via detail::RegisterActorsTab(). Reads the
// game-thread-built actor snapshot through detail::Actors(); the producing tick
// lives in OverlayConsole::Init.

#include "OverlayTabs.h"
#ifndef NOMINMAX
#define NOMINMAX               // StringLib pulls <Windows.h>; keep std::min/max usable
#endif
#include "StringLib.h"         // IEquals / IContains (canonical CI helpers)

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
            if (ImGui::Button("Refresh")) g_actors.request();
            ImGui::SameLine();
            bool autoOn = g_actors.isAuto();
            if (ImGui::Checkbox("Auto", &autoOn)) g_actors.setAuto(autoOn);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.f);
            ImGui::Combo("##rep", &repMode, "net: all\0replicated\0not replicated\0");

            ImGui::SameLine();
            ImGui::TextDisabled("(right-click a header to show/hide columns)");

            // Per-field filters (one input each).
            ImGui::SetNextItemWidth(90.f);
            ImGui::Combo("##cmode", &classMode, "contains\0is\0is+sub\0not\0not+sub\0");
            ImGui::SameLine(); ImGui::SetNextItemWidth(150.f);
            ImGui::InputTextWithHint("##fclass", "class", fClass, sizeof(fClass));
            ImGui::SameLine(); ImGui::SetNextItemWidth(130.f);
            ImGui::InputTextWithHint("##fname", "name", fName, sizeof(fName));
            ImGui::SameLine(); ImGui::SetNextItemWidth(120.f);
            ImGui::InputTextWithHint("##fouter", "outer", fOuter, sizeof(fOuter));
            ImGui::SameLine(); ImGui::SetNextItemWidth(120.f);
            ImGui::InputTextWithHint("##fowner", "owner", fOwner, sizeof(fOwner));
            ImGui::SameLine(); ImGui::SetNextItemWidth(-1.f);
            ImGui::InputTextWithHint("##finst", "instigator", fInst, sizeof(fInst));

            std::vector<ActorList::Row> rows = g_actors.read();
            if (rows.empty()) g_actors.request();   // populate on first view

            const std::string fNameF = fName, fOuterF = fOuter, fOwnerF = fOwner, fInstF = fInst;

            size_t shown = 0;
            const ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
                                     | ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable
                                     | ImGuiTableFlags_Hideable;   // right-click header → show/hide (persists via ini)
            const float actFooter = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
            if (ImGui::BeginTable("##actors", 7, tf, ImVec2(0, -actFooter)))
            {
                ImGui::TableSetupColumn("class",      ImGuiTableColumnFlags_WidthStretch, 0, 0);
                ImGui::TableSetupColumn("name",       ImGuiTableColumnFlags_WidthStretch, 0, 1);
                ImGui::TableSetupColumn("net",        ImGuiTableColumnFlags_WidthFixed,  40, 2);
                ImGui::TableSetupColumn("outer",      ImGuiTableColumnFlags_WidthStretch, 0, 5);
                ImGui::TableSetupColumn("owner",      ImGuiTableColumnFlags_WidthStretch, 0, 3);
                ImGui::TableSetupColumn("instigator", ImGuiTableColumnFlags_WidthStretch, 0, 4);
                // addr = unique instance id; off by default but available (hideable).
                ImGui::TableSetupColumn("addr",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 120, 6);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

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
                if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs())
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
                        ImGui::TableNextRow();
                        ImGui::PushID(idx[row]);

                        const bool sel = (r.addr && r.addr == actorSelAddr);
                        if (sel) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 90, 130, 120));

                        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.className.c_str());

                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::Selectable(r.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
                            actorSelAddr = r.addr;

                        ImGui::TableSetColumnIndex(2);
                        if (r.replicated) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.f), "rep");
                        else              ImGui::TextDisabled("-");

                        ImGui::TableSetColumnIndex(3);
                        if (r.outerAddr)      { if (ImGui::SmallButton(r.outer.c_str()))      jumpTo(r.outerAddr); }
                        else                    ImGui::TextDisabled("-");
                        ImGui::TableSetColumnIndex(4);
                        if (r.ownerAddr)      { if (ImGui::SmallButton(r.owner.c_str()))      jumpTo(r.ownerAddr); }
                        else                    ImGui::TextDisabled("-");
                        ImGui::TableSetColumnIndex(5);
                        if (r.instigatorAddr) { if (ImGui::SmallButton(r.instigator.c_str())) jumpTo(r.instigatorAddr); }
                        else                    ImGui::TextDisabled("-");

                        ImGui::TableSetColumnIndex(6);
                        ImGui::Text("%llX", (unsigned long long)r.addr);

                        if (gotoPos == row) ImGui::SetScrollHereY(0.5f);
                        ImGui::PopID();
                    }
                if (gotoPos >= 0) actorGotoAddr = 0;   // resolved; a fresh click stays pending
                ImGui::EndTable();
            }
            ImGui::TextDisabled("%zu shown / %zu actors", shown, rows.size());
            }
        };
    } // anonymous namespace

    void detail::RegisterActorsTab() { static ActorsTab s; (void)s; }
}
