// Lib_OverlayConsole.cpp — in-game CLI: log pane + console + command list.

#include "Lib_OverlayConsole.h"
#include "Lib_Overlay.h"
#include "Lib_CommandHandler.h"
#include "Lib_GameHooks.h"     // EnqueueOnce, EnqueueEveryNTicks
#include "Lib_VarSystem.h"     // g_Vars snapshot for the Vars tab
#include "Lib_KeyBindings.h"   // SnapshotCli for the Keybinds tab
#include "Lib_ActorList.h"     // actor snapshot for the Actors tab
#include "Lib_NetLogConfig.h"  // ConfigPath for the Config tab
#ifndef NOMINMAX
#define NOMINMAX               // StringLib pulls <Windows.h>; keep std::min/max usable
#endif
#include "StringLib.h"         // IEquals / IContains (canonical CI helpers)
#include "CoreUtils.h"         // SafeStof / SafeStoll (canonical parsers)
#include "GameThreadSnapshot.h" // double-buffered game-thread→UI snapshot
#include "Common.h"

#include <fstream>

#include <imgui.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace OverlayConsole
{
    namespace
    {
        // ── Log ring buffer (written by the logger thread, read on the overlay) ──
        struct LogLine { spdlog::level::level_enum level; std::string text; };
        std::mutex            g_logMutex;
        std::deque<LogLine>   g_log;
        constexpr size_t      kMaxLines = 4000;
        // Monotonic count of lines ever pushed. g_log.size() saturates at kMaxLines
        // (each push pops the front once full), so it can't signal "new content" —
        // this can, and is what auto-scroll keys off.
        uint64_t              g_logSeq = 0;

        // ImGui's default font (ProggyClean) only carries ASCII glyphs, so the
        // UTF-8 box-drawing / typographic punctuation we log (─ — • → …) renders as
        // missing-glyph garbage. Fold the common offenders to ASCII; replace any
        // other non-ASCII codepoint with '?'. One choke point covers logs,
        // responses, echoes and completions.
        const char* FoldCodepoint(uint32_t cp)
        {
            // Box-drawing block (U+2500–U+257F), single AND double line: verticals →
            // '|', horizontals → '-', everything else (corners/tees/crosses) → '+'.
            // The scan/property dumps lean on these (╔ ═ ║ ╠ ├ └ │ ─ …).
            if (cp >= 0x2500 && cp <= 0x257F)
            {
                switch (cp)
                {
                case 0x2502: case 0x2503: case 0x2506: case 0x2507:
                case 0x250A: case 0x250B: case 0x2551:           // │ ┃ ┆ ┇ ┊ ┋ ║
                    return "|";
                case 0x2500: case 0x2501: case 0x2504: case 0x2505:
                case 0x2508: case 0x2509: case 0x2550:           // ─ ━ ┄ ┅ ┈ ┉ ═
                    return "-";
                default:
                    return "+";                                  // ╔ ╗ ╚ ╝ ╠ ╣ ┌ ├ └ ┼ …
                }
            }
            switch (cp)
            {
            case 0x2012: case 0x2013: case 0x2014: case 0x2015: // figure/en/em/horiz dash
                return "-";
            case 0x2018: case 0x2019: return "'";               // ‘ ’
            case 0x201C: case 0x201D: return "\"";              // “ ”
            case 0x2022: case 0x00B7: return "*";               // • ·
            case 0x2026: return "...";                          // …
            case 0x2192: return "->";  case 0x2190: return "<-";
            case 0x2191: return "^";   case 0x2193: return "v";
            case 0x00A0: return " ";                            // nbsp
            default:     return "?";
            }
        }

        std::string SanitizeForImGui(const std::string& in)
        {
            std::string out;
            out.reserve(in.size());
            for (size_t i = 0; i < in.size(); )
            {
                unsigned char c = (unsigned char)in[i];
                if (c < 0x80) { out += (char)c; ++i; continue; }
                int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
                if (extra < 0 || i + extra >= in.size()) { out += '?'; ++i; continue; }
                uint32_t cp = c & (0x7F >> extra);
                bool ok = true;
                for (int k = 1; k <= extra; ++k)
                {
                    unsigned char cc = (unsigned char)in[i + k];
                    if ((cc & 0xC0) != 0x80) { ok = false; break; }
                    cp = (cp << 6) | (cc & 0x3F);
                }
                if (!ok) { out += '?'; ++i; continue; }
                out += FoldCodepoint(cp);
                i += extra + 1;
            }
            return out;
        }

        // Store one VISUAL line per LogLine. A single log message may carry embedded
        // newlines (multi-line function signatures, etc.); ImGuiListClipper assumes a
        // uniform item height, so a multi-line entry would corrupt its scroll-height
        // math (auto-scroll then can't reach the true bottom). Split on '\n'.
        // contIndent: spaces to prepend to continuation lines (lines after the first)
        // of a multi-line message, so a wrapped dump hangs under the first line's text
        // instead of starting at column 0 with no log prefix. The sink passes the
        // formatted-prefix width; the response tap passes 0.
        void PushLine(spdlog::level::level_enum lvl, std::string text, size_t contIndent = 0)
        {
            // Drop the trailing newline (spdlog's formatter appends one) so the split
            // below doesn't yield a spurious empty final line — that double-spaced the
            // whole log. Internal blank lines are intentional and preserved.
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            std::lock_guard lk(g_logMutex);
            size_t start = 0;
            bool   first = true;
            while (start <= text.size())
            {
                size_t nl  = text.find('\n', start);
                size_t end = (nl == std::string::npos) ? text.size() : nl;
                std::string seg = text.substr(start, end - start);
                while (!seg.empty() && seg.back() == '\r') seg.pop_back();
                if (!first && contIndent) seg.insert(0, contIndent, ' ');
                g_log.push_back({ lvl, SanitizeForImGui(seg) });
                if (g_log.size() > kMaxLines) g_log.pop_front();
                ++g_logSeq;
                first = false;
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }

        // ── spdlog sink → ring buffer ────────────────────────────────────────────
        template<typename Mutex>
        class overlay_sink : public spdlog::sinks::base_sink<Mutex>
        {
        protected:
            void sink_it_(const spdlog::details::log_msg& msg) override
            {
                spdlog::memory_buf_t buf;
                this->formatter_->format(msg, buf);
                std::string full(buf.data(), buf.size());
                // Prefix width = where the raw payload starts within the formatted
                // line ("[time] [name] [level] " before %v). Continuation lines of a
                // multi-line message are indented by this so they align under the text.
                std::string payload(msg.payload.data(), msg.payload.size());
                size_t at = payload.empty() ? std::string::npos : full.find(payload);
                size_t prefixLen = (at == std::string::npos) ? 0 : at;
                PushLine(msg.level, std::move(full), prefixLen);
            }
            void flush_() override {}
        };
        using overlay_sink_mt = overlay_sink<std::mutex>;

        spdlog::sink_ptr               g_sink;
        std::atomic<CommandHandler*>   g_handler{ nullptr };

        // ── VarSystem snapshot (game thread writes, overlay reads) ───────────────
        // g_Vars is owned by the game thread; iterating it from the overlay thread
        // would race a concurrent insert/rehash. A periodic game-thread tick copies
        // it into g_vars; the Vars tab reads only this copy and pushes edits back as
        // `set <name> <value>` commands (game-thread safe).
        struct VarSnap { std::string name, token; VarSystem::VarType type; };
        GameThreadSnapshot<std::vector<VarSnap>> g_vars;   // refreshes unconditionally
        // Persistent per-row edit state so dragging/typing isn't clobbered by the
        // 0.5 s snapshot refresh while a widget is active.
        std::unordered_map<std::string, float>       g_varEditF;
        std::unordered_map<std::string, int>         g_varEditI;
        std::unordered_map<std::string, std::string> g_varEditS;

        // Keybinds tab: per-row chord edit buffer (idle-seeded), plus the add-row.
        std::unordered_map<std::string, std::string> g_kbChordEdit;
        char g_kbAddChord[64]  = {};
        char g_kbAddCmd[256]   = {};

        // Vars tab add-row.
        char g_varAddName[128] = {};
        char g_varAddVal[256]  = {};

        // Actors tab: game-thread-built snapshot. Heavy GObjects walk, so the snapshot
        // gates auto-refresh on a recent beat() (tab actually rendered) to avoid spiking
        // frametime when you aren't looking at the tab.
        GameThreadSnapshot<std::vector<ActorList::Row>> g_actors;

        uint64_t NowMs()
        {
            using namespace std::chrono;
            return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }
        char  g_fClass[128] = {}, g_fName[128] = {}, g_fOuter[128] = {}, g_fOwner[128] = {}, g_fInst[128] = {};
        int   g_classMode = 0;          // 0 Contains, 1 Is, 2 Is+Sub, 3 Not, 4 NotSub
        int   g_repMode   = 0;          // 0 All, 1 Replicated, 2 Not replicated
        uint64_t    g_actorGotoAddr = 0;   // pending jump-to actor (by unique addr)
        uint64_t    g_actorSelAddr  = 0;   // highlighted row (by unique addr)

        // Config tab: raw config.yaml editor.
        char        g_cfgText[32768] = {};
        bool        g_cfgLoaded = false;
        std::string g_cfgStatus;

        // ── UI state (overlay thread only) ───────────────────────────────────────
        char        g_input[512]   = {};
        char        g_filter[128]  = {};
        char        g_logFilter[128] = {};   // Console log-pane live filter (substring, case-insensitive)
        char        g_args[512]    = {};   // args for the selected command
        std::string g_selCmd;             // currently selected command (Commands tab)
        std::string g_selDesc;
        bool g_autoScroll    = true;
        bool g_scrollBottom  = false;

        // Console input history (most-recent last). g_historyPos walks it: -1 means
        // "at the live edit line"; 0..n-1 index into g_history from newest to oldest.
        std::vector<std::string> g_history;
        int                      g_historyPos = -1;

        // Live autocomplete candidates for the console input (shown in a wrapped
        // pane). Derived from the input every frame: present whenever there's text;
        // when the box is empty they appear only after Tab (g_acShowAll).
        std::vector<std::string> g_acMatches;
        std::string              g_acPrefix;            // buffer text kept before the completed token (e.g. "set ")
        bool                     g_acShowAll = false;   // Tab-on-empty → list everything
        bool                     g_focusInput = false;  // re-grab input focus next frame (after a click)
        // Last g_logSeq the draw loop observed; auto-scroll pins only when this
        // changes (new lines arrived), so idle scroll-up works with it checked.
        uint64_t                 g_lastLogSeq = 0;
        // Pin-to-bottom countdown. GetScrollMaxY() lags one frame behind content
        // growth (ContentSize is finalized at End), so a single same-frame pin lands
        // a batch short; hold the pin a couple extra frames to settle on the bottom.
        int                      g_pinFrames = 0;

        ImVec4 LevelColor(spdlog::level::level_enum lvl)
        {
            switch (lvl)
            {
            case spdlog::level::warn:     return ImVec4(1.00f, 0.80f, 0.25f, 1.f);
            case spdlog::level::err:
            case spdlog::level::critical: return ImVec4(1.00f, 0.40f, 0.40f, 1.f);
            case spdlog::level::debug:
            case spdlog::level::trace:    return ImVec4(0.60f, 0.60f, 0.60f, 1.f);
            default:                      return ImVec4(0.86f, 0.86f, 0.86f, 1.f);
            }
        }

        void RunCommand(std::string line)
        {
            if (line.empty()) return;
            PushLine(spdlog::level::info, "> " + line);   // echo
            // Record in history (drop a consecutive duplicate of the last entry).
            if (g_history.empty() || g_history.back() != line) g_history.push_back(line);
            g_historyPos = -1;
            g_acMatches.clear();
            g_acShowAll = false;
            EnqueueOnce([line]
            {
                if (auto* h = g_handler.load()) h->Dispatch(line, 0);
            });
        }

        // Longest common prefix of a set of command names (case-sensitive).
        std::string CommonPrefix(const std::vector<std::string>& v)
        {
            if (v.empty()) return {};
            std::string p = v.front();
            for (size_t i = 1; i < v.size(); ++i)
            {
                size_t n = 0;
                while (n < p.size() && n < v[i].size() && p[n] == v[i][n]) ++n;
                p.resize(n);
            }
            return p;
        }

        // InputText callback: Tab-completion on the command word + Up/Down history.
        int InputCallback(ImGuiInputTextCallbackData* data)
        {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
            {
                // Empty box + Tab → list everything (the variants pane is derived live).
                if (data->BufTextLen == 0) { g_acShowAll = true; return 0; }
                if (g_acMatches.empty()) return 0;

                // g_acMatches/g_acPrefix were computed this frame from the buffer.
                // Advance the completed token to the longest common prefix (or full if
                // unique), keeping g_acPrefix (e.g. "set ") for argument completions.
                const std::string comp = g_acMatches.size() == 1 ? g_acMatches.front()
                                                                 : CommonPrefix(g_acMatches);
                const std::string full = g_acPrefix + comp;
                if (full.size() > (size_t)data->BufTextLen)
                {
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, full.c_str());
                }
            }
            else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
            {
                if (g_history.empty()) return 0;
                const int n = (int)g_history.size();
                if (data->EventKey == ImGuiKey_UpArrow)
                {
                    if (g_historyPos < n - 1) ++g_historyPos;
                }
                else if (data->EventKey == ImGuiKey_DownArrow)
                {
                    if (g_historyPos >= 0) --g_historyPos;
                }
                data->DeleteChars(0, data->BufTextLen);
                if (g_historyPos >= 0)
                    data->InsertChars(0, g_history[n - 1 - g_historyPos].c_str());  // newest = pos 0
            }
            return 0;
        }

        // CI string helpers live in StringLib: IEquals / IContains.
        using StringLib::IEquals;
        using StringLib::IContains;

        // Inline ghost hint (like the CLI input): the suffix of the alphabetically
        // first command that extends the current command word, or "" if none. Only
        // hints the command token — once there's a space, the user is typing args.
        std::string GhostSuffix()
        {
            auto* h = g_handler.load();
            if (!h || g_input[0] == '\0') return {};
            std::string buf = g_input;
            if (buf.find(' ') != std::string::npos) return {};
            const std::string* best = nullptr;
            for (const auto& [name, entry] : h->GetEntries())
                if (name.size() > buf.size() && name.compare(0, buf.size(), buf) == 0)
                    if (!best || name < *best) best = &name;
            return best ? best->substr(buf.size()) : std::string{};
        }

        // Width an autocomplete chip occupies (Selectable = text + frame padding).
        float AcChipWidth(const std::string& m)
        {
            return ImGui::CalcTextSize(m.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.f;
        }

        // Rows g_acMatches wraps into at content width w (exact flow simulation; used
        // both to size the pane and to lay it out, so they always agree).
        int AcFlowRows(float w)
        {
            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float x = 0.f; int rows = 1;
            for (const auto& m : g_acMatches)
            {
                const float iw = AcChipWidth(m);
                if (x > 0.f && x + sp + iw > w) { rows++; x = iw; }
                else                            { x += (x > 0.f ? sp : 0.f) + iw; }
            }
            return rows;
        }

        // Candidate values for the first argument of commands whose args are known.
        std::vector<std::string> ArgPoolFor(const std::string& cmd)
        {
            if (cmd == "set" || cmd == "get" || cmd == "unset")
            {
                return g_vars.with([](const std::vector<VarSnap>& snap) {
                    std::vector<std::string> v;
                    for (const auto& s : snap) v.push_back(s.name);
                    return v;
                });
            }
            if (cmd == "unbind")
            {
                std::vector<std::string> v;
                for (const auto& b : KeyBindings::SnapshotCli()) v.push_back(b.chord);
                return v;
            }
            return {};
        }

        // Recompute g_acMatches (+ g_acPrefix) from the current input every frame:
        //   empty            → all commands, but only after Tab (g_acShowAll)
        //   one word         → command-name prefix matches
        //   "cmd <arg>"      → first-arg value matches for known cmds (var/chord names)
        // g_acPrefix is the text kept before the completed token (e.g. "set ").
        void ComputeCompletions()
        {
            g_acMatches.clear();
            g_acPrefix.clear();
            auto* h = g_handler.load();
            const std::string buf = g_input;

            if (buf.empty())
            {
                if (g_acShowAll && h)
                    for (const auto& [name, e] : h->GetEntries()) g_acMatches.push_back(name);
                std::sort(g_acMatches.begin(), g_acMatches.end());
                return;
            }
            g_acShowAll = false;

            const size_t sp = buf.find(' ');
            if (sp == std::string::npos)
            {
                if (h)
                    for (const auto& [name, e] : h->GetEntries())
                        if (name.size() >= buf.size() && name.compare(0, buf.size(), buf) == 0)
                            g_acMatches.push_back(name);
            }
            else if (buf.find(' ', sp + 1) == std::string::npos)   // exactly one arg being typed
            {
                const std::string cmd       = buf.substr(0, sp);
                const std::string argPrefix = buf.substr(sp + 1);
                g_acPrefix = buf.substr(0, sp + 1);                // "cmd "
                for (const auto& s : ArgPoolFor(cmd))
                    if (s.size() >= argPrefix.size() && s.compare(0, argPrefix.size(), argPrefix) == 0)
                        g_acMatches.push_back(s);
            }
            std::sort(g_acMatches.begin(), g_acMatches.end());
        }

        void DrawConsoleTab()
        {
            // Filter row (CLI Ctrl+F parity): live substring filter over the log pane.
            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputTextWithHint("##logfilter", "filter log (substring)...", g_logFilter, sizeof(g_logFilter));
            std::string lfilter = g_logFilter;

            // Derive the variants pane from the current input every frame: command
            // names, or first-arg values for commands whose args are known.
            ComputeCompletions();

            const float inputRowH = ImGui::GetFrameHeightWithSpacing() + 4.f;
            const float availH    = ImGui::GetContentRegionAvail().y;   // for log + variants + input
            // Size the variants pane to the candidates: estimate wrapped rows from
            // total glyphs / glyphs-per-line (monospace font), clamped [1, 12]; it
            // scrolls past the cap. Then cap to ~45% of the space left after the input
            // row so the log keeps the majority even in a short window (the pane
            // scrolls internally rather than starving the log).
            float acH = 0.f;
            // Inner content width of the ##ac child (panel width minus its WindowPadding
            // and border); used by both the height estimate and the layout so they match.
            const ImGuiStyle& st = ImGui::GetStyle();
            const float acInnerW = ImGui::GetContentRegionAvail().x - st.WindowPadding.x * 2.f - 2.f;
            if (!g_acMatches.empty())
            {
                const int rows = std::min(AcFlowRows(acInnerW), 12);
                // One Selectable row = text line + ItemSpacing between rows. Add the
                // child's chrome: WindowPadding (both sides), border, and the
                // ItemSpacing subtracted when the child is created below.
                const float chromeH = st.WindowPadding.y * 2.f + st.ItemSpacing.y + 2.f;
                acH = rows * ImGui::GetTextLineHeight() + (rows - 1) * st.ItemSpacing.y + chromeH;
                const float acMax = std::max(ImGui::GetTextLineHeight() + chromeH,
                                             (availH - inputRowH) * 0.45f);
                acH = std::min(acH, acMax);
            }
            const float footer = inputRowH + acH;
            if (ImGui::BeginChild("##log", ImVec2(0, -footer), true, ImGuiWindowFlags_HorizontalScrollbar))
            {
                std::lock_guard lk(g_logMutex);
                // Only auto-pin when new lines actually arrived this frame; otherwise
                // the user can't scroll up to read history while Auto-scroll is on.
                const bool grew = g_logSeq != g_lastLogSeq;
                g_lastLogSeq = g_logSeq;
                if (lfilter.empty())
                {
                    ImGuiListClipper clipper;
                    clipper.Begin((int)g_log.size());
                    while (clipper.Step())
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        {
                            const LogLine& l = g_log[i];
                            ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(l.level));
                            ImGui::TextUnformatted(l.text.c_str());
                            ImGui::PopStyleColor();
                        }
                }
                else
                {
                    // Filtered: build the matching index set, then clip over that.
                    static std::vector<int> matches;
                    matches.clear();
                    for (int i = 0; i < (int)g_log.size(); ++i)
                        if (IContains(g_log[i].text, lfilter)) matches.push_back(i);
                    ImGuiListClipper clipper;
                    clipper.Begin((int)matches.size());
                    while (clipper.Step())
                        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
                        {
                            const LogLine& l = g_log[matches[r]];
                            ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(l.level));
                            ImGui::TextUnformatted(l.text.c_str());
                            ImGui::PopStyleColor();
                        }
                }
                // Arm the pin when new content arrived (grew) or right after running
                // a command; hold it a couple frames so ScrollMax (one frame behind)
                // catches up to the real bottom. Idle scroll-up still works.
                if (g_scrollBottom || (g_autoScroll && grew)) g_pinFrames = 2;
                if (g_pinFrames > 0) { ImGui::SetScrollY(ImGui::GetScrollMaxY()); --g_pinFrames; }
                g_scrollBottom = false;
            }
            ImGui::EndChild();

            // Autocomplete candidates — a horizontal flow of Selectables: hover
            // highlights, click inserts the command into the input. Refreshes live.
            if (!g_acMatches.empty())
            {
                if (ImGui::BeginChild("##ac", ImVec2(0, acH - st.ItemSpacing.y), true))
                {
                    const float w  = ImGui::GetContentRegionAvail().x;
                    const float sp = st.ItemSpacing.x;
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.80f, 1.0f, 1.f));
                    float x = 0.f;
                    for (const auto& m : g_acMatches)
                    {
                        const float iw = AcChipWidth(m);
                        if (x > 0.f && x + sp + iw > w) x = 0.f;   // wrap to a new row
                        if (x > 0.f) ImGui::SameLine();
                        if (ImGui::Selectable(m.c_str(), false, 0, ImVec2(iw, 0.f)))
                        {
                            // Keep the prefix (e.g. "set ") and insert the chosen token.
                            strncpy_s(g_input, sizeof(g_input), (g_acPrefix + m).c_str(), _TRUNCATE);
                            g_focusInput = true;   // jump back into the input box
                        }
                        x += (x > 0.f ? sp : 0.f) + iw;
                    }
                    ImGui::PopStyleColor();
                }
                ImGui::EndChild();
            }

            if (g_focusInput) { ImGui::SetKeyboardFocusHere(); g_focusInput = false; }
            ImGui::SetNextItemWidth(-255.f);
            const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue
                                            | ImGuiInputTextFlags_CallbackCompletion   // Tab
                                            | ImGuiInputTextFlags_CallbackHistory;     // Up/Down
            bool enter = ImGui::InputText("##cmd", g_input, sizeof(g_input), flags, InputCallback);
            // Inline ghost hint: grey suffix drawn right after the typed text. Only
            // while the box is focused and the caret is at the end (no mid-edit).
            if (ImGui::IsItemFocused())
            {
                std::string ghost = GhostSuffix();
                if (!ghost.empty())
                {
                    const ImVec2 mn  = ImGui::GetItemRectMin();
                    const ImVec2 pad = ImGui::GetStyle().FramePadding;
                    const float  tw  = ImGui::CalcTextSize(g_input).x;
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(mn.x + pad.x + tw, mn.y + pad.y),
                        IM_COL32(120, 120, 120, 255), ghost.c_str());
                }
            }
            ImGui::SameLine();
            bool run = ImGui::Button("Run");
            ImGui::SameLine();
            if (ImGui::Button("Clear")) { std::lock_guard lk(g_logMutex); g_log.clear(); }
            ImGui::SameLine();
            if (ImGui::Button("Copy"))   // copy the currently-visible (filtered) log
            {
                std::string out;
                {
                    std::lock_guard lk(g_logMutex);
                    for (const auto& l : g_log)
                        if (lfilter.empty() || IContains(l.text, lfilter))
                        {
                            out += l.text;
                            out += '\n';
                        }
                }
                ImGui::SetClipboardText(out.c_str());
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &g_autoScroll);

            if (enter || run)
            {
                RunCommand(g_input);
                g_input[0] = '\0';
                g_scrollBottom = true;
                ImGui::SetKeyboardFocusHere(-1);   // keep focus in the input box
            }
        }

        void DrawCommandsTab()
        {
            auto* h = g_handler.load();
            if (!h) { ImGui::TextDisabled("(no command handler bound)"); return; }

            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputTextWithHint("##filter", "filter commands...", g_filter, sizeof(g_filter));
            std::string filter = g_filter;
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
                                + (g_selCmd.empty() ? 0.f : ImGui::GetFrameHeightWithSpacing());

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
                            if (ImGui::Selectable(name.c_str(), g_selCmd == name))
                            {
                                g_selCmd = name;       // select → args footer targets it
                                g_selDesc = desc;
                                g_args[0] = '\0';
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
            if (g_selCmd.empty())
            {
                ImGui::TextDisabled("Select a command to pass arguments, or use Run for no-arg commands.");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.f), "%s", g_selCmd.c_str());
                if (!g_selDesc.empty())
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("— %s", g_selDesc.c_str());
                }
                ImGui::SetNextItemWidth(-60.f);
                bool enter = ImGui::InputTextWithHint("##args", "arguments (e.g. 0 0 0)...",
                                                      g_args, sizeof(g_args), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if (ImGui::Button("Run##sel") || enter)
                {
                    std::string line = g_selCmd;
                    if (g_args[0]) { line += ' '; line += g_args; }
                    RunCommand(line);
                }
            }
        }

        // Commit a var change through the normal command path (game-thread safe).
        void SetVar(const std::string& name, const std::string& value)
        {
            RunCommand("set " + name + " " + value);
        }

        void DrawVarsTab()
        {
            std::vector<VarSnap> vars = g_vars.read();
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
                        bool b = (v.token == "true" || v.token == "1" || v.token == "True");
                        if (ImGui::Checkbox("##b", &b)) SetVar(v.name, b ? "true" : "false");
                        break;
                    }
                    case VT::Float:
                    {
                        float& f = g_varEditF[v.name];
                        ImGui::DragFloat("##f", &f, 0.1f);
                        if (ImGui::IsItemDeactivatedAfterEdit()) SetVar(v.name, std::to_string(f));
                        if (!ImGui::IsItemActive()) f = SafeStof(v.token);
                        break;
                    }
                    case VT::Int32:
                    {
                        int& i = g_varEditI[v.name];
                        ImGui::DragInt("##i", &i);
                        if (ImGui::IsItemDeactivatedAfterEdit()) SetVar(v.name, std::to_string(i));
                        if (!ImGui::IsItemActive()) i = (int)SafeStoll(v.token);
                        break;
                    }
                    default:   // String / Vector / Rotator / Name / Object → editable text
                    {
                        std::string& s = g_varEditS[v.name];
                        char buf[256];
                        strncpy_s(buf, sizeof(buf), s.c_str(), _TRUNCATE);
                        if (ImGui::InputText("##s", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
                            SetVar(v.name, buf);
                        s = buf;
                        if (!ImGui::IsItemActive()) s = v.token;
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
            ImGui::InputTextWithHint("##addvname", "name", g_varAddName, sizeof(g_varAddName));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-90.f);
            bool addEnter = ImGui::InputTextWithHint("##addvval", "value", g_varAddVal, sizeof(g_varAddVal),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("Set##addv") || addEnter) && g_varAddName[0] && g_varAddVal[0])
            {
                RunCommand(std::string("set ") + g_varAddName + " " + g_varAddVal);
                g_varAddName[0] = '\0';
                g_varAddVal[0]  = '\0';
            }
        }

        void DrawKeybindsTab()
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
                if (!ImGui::IsItemActive()) strncpy_s(s_otk, sizeof(s_otk), cur.c_str(), _TRUNCATE);
                ImGui::SameLine();
                if ((ImGui::Button("Set##otk") || ed) && s_otk[0])
                {
                    Key k; Mod m;
                    if (KeyBindings::ParseChord(s_otk, k, m)) Overlay::SetToggleKey((uint16_t)k);
                    else info("[overlay] unknown key '{}'", s_otk);
                }
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
                        std::string& s = g_kbChordEdit[b.chord];
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
            ImGui::InputTextWithHint("##addchord", "key (e.g. F3)", g_kbAddChord, sizeof(g_kbAddChord));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-90.f);
            bool addEnter = ImGui::InputTextWithHint("##addcmd", "command...", g_kbAddCmd, sizeof(g_kbAddCmd),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("Bind") || addEnter) && g_kbAddChord[0] && g_kbAddCmd[0])
            {
                RunCommand(std::string("bind ") + g_kbAddChord + " " + g_kbAddCmd);
                g_kbAddChord[0] = '\0';
                g_kbAddCmd[0]   = '\0';
            }
        }

        // Does row r pass the class-filter (mode + target text)?
        bool ClassFilterPass(const ActorList::Row& r)
        {
            const std::string t = g_fClass;
            if (t.empty()) return true;
            const std::string token = "/" + t + "/";   // for subclass chain match
            switch (g_classMode)
            {
            case 0: return IContains(r.className, t);                 // Contains
            case 1: return IEquals(r.className, t);                 // Is (exact)
            case 2: return IContains(r.classChain, token);           // Is or subclass
            case 3: return !IEquals(r.className, t);                // Not (exact)
            case 4: return !IContains(r.classChain, token);          // Not class or subclasses
            }
            return true;
        }

        void DrawActorsTab()
        {
            g_actors.beat(NowMs());   // heartbeat: auto-refresh only runs while this is live

            // ── Controls + per-field filters ─────────────────────────────────────
            if (ImGui::Button("Refresh")) g_actors.request();
            ImGui::SameLine();
            bool autoOn = g_actors.isAuto();
            if (ImGui::Checkbox("Auto", &autoOn)) g_actors.setAuto(autoOn);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.f);
            ImGui::Combo("##rep", &g_repMode, "net: all\0replicated\0not replicated\0");

            ImGui::SameLine();
            ImGui::TextDisabled("(right-click a header to show/hide columns)");

            // Per-field filters (one input each).
            ImGui::SetNextItemWidth(90.f);
            ImGui::Combo("##cmode", &g_classMode, "contains\0is\0is+sub\0not\0not+sub\0");
            ImGui::SameLine(); ImGui::SetNextItemWidth(150.f);
            ImGui::InputTextWithHint("##fclass", "class", g_fClass, sizeof(g_fClass));
            ImGui::SameLine(); ImGui::SetNextItemWidth(130.f);
            ImGui::InputTextWithHint("##fname", "name", g_fName, sizeof(g_fName));
            ImGui::SameLine(); ImGui::SetNextItemWidth(120.f);
            ImGui::InputTextWithHint("##fouter", "outer", g_fOuter, sizeof(g_fOuter));
            ImGui::SameLine(); ImGui::SetNextItemWidth(120.f);
            ImGui::InputTextWithHint("##fowner", "owner", g_fOwner, sizeof(g_fOwner));
            ImGui::SameLine(); ImGui::SetNextItemWidth(-1.f);
            ImGui::InputTextWithHint("##finst", "instigator", g_fInst, sizeof(g_fInst));

            std::vector<ActorList::Row> rows = g_actors.read();
            if (rows.empty()) g_actors.request();   // populate on first view

            const std::string fName = g_fName, fOuter = g_fOuter, fOwner = g_fOwner, fInst = g_fInst;

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
                    if (g_repMode == 1 && !r.replicated) continue;
                    if (g_repMode == 2 &&  r.replicated) continue;
                    if (!ClassFilterPass(r))                            continue;
                    if (!fName.empty()  && !IContains(r.name,  fName)) continue;
                    if (!fOuter.empty() && !IContains(r.outer, fOuter))continue;
                    if (!fOwner.empty() && !IContains(r.owner, fOwner))continue;
                    if (!fInst.empty()  && !IContains(r.instigator, fInst)) continue;
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
                if (g_actorGotoAddr)
                    for (int k = 0; k < (int)idx.size(); ++k)
                        if (rows[idx[k]].addr == g_actorGotoAddr) { gotoPos = k; break; }

                // Jump targets a specific instance by addr; clear filters so it's
                // reachable next frame regardless of the current view.
                auto jumpTo = [](uint64_t targetAddr)
                {
                    if (!targetAddr) return;
                    g_actorGotoAddr = targetAddr; g_actorSelAddr = targetAddr;
                    g_fName[0] = g_fOuter[0] = g_fOwner[0] = g_fInst[0] = g_fClass[0] = '\0';
                    g_repMode = 0;
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

                        const bool sel = (r.addr && r.addr == g_actorSelAddr);
                        if (sel) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 90, 130, 120));

                        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.className.c_str());

                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::Selectable(r.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
                            g_actorSelAddr = r.addr;

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
                if (gotoPos >= 0) g_actorGotoAddr = 0;   // resolved; a fresh click stays pending
                ImGui::EndTable();
            }
            ImGui::TextDisabled("%zu shown / %zu actors", shown, rows.size());
        }

        // ImGui has no real tab stops — it renders '\t' as a fixed advance of 4 space
        // widths. Matching that here makes the tab→space conversion visually identical.
        constexpr int kTabWidth = 4;

        // YAML forbids tabs for indentation, but the editor allows typing/pasting them
        // (ImGuiInputTextFlags_AllowTabInput). Expand each '\t' to kTabWidth spaces so a
        // Save can't write an unparseable file.
        std::string ExpandTabs(const char* s)
        {
            std::string out;
            for (const char* p = s; *p; ++p)
            {
                if (*p == '\t') out.append(kTabWidth, ' ');
                else            out.push_back(*p);
            }
            return out;
        }

        void ConfigLoad()
        {
            g_cfgLoaded = true;
            g_cfgText[0] = '\0';
            const std::string path = NetLogConfig::ConfigPath();
            if (path.empty()) { g_cfgStatus = "config path unresolved"; return; }
            std::ifstream f(path, std::ios::binary);
            if (!f) { g_cfgStatus = "not found (Save creates it): " + path; return; }
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (text.size() >= sizeof(g_cfgText)) { g_cfgStatus = "file too large to edit here"; return; }
            std::copy(text.begin(), text.end(), g_cfgText);
            g_cfgText[text.size()] = '\0';
            g_cfgStatus = "loaded " + path;
        }

        void ConfigSave()
        {
            const std::string path = NetLogConfig::ConfigPath();
            if (path.empty()) { g_cfgStatus = "config path unresolved"; return; }
            const std::string text = ExpandTabs(g_cfgText);   // YAML safety: no literal tabs
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (!f) { g_cfgStatus = "save FAILED: " + path; return; }
            f << text;
            g_cfgStatus = "saved " + path;
            // Reflect the normalized text back into the editor so the buffer matches disk
            // (only when it still fits the fixed-size buffer).
            if (text.size() < sizeof(g_cfgText))
            {
                std::copy(text.begin(), text.end(), g_cfgText);
                g_cfgText[text.size()] = '\0';
            }
        }

        void DrawConfigTab()
        {
            if (!g_cfgLoaded) ConfigLoad();

            if (ImGui::Button("Save"))   ConfigSave();
            ImGui::SameLine();
            if (ImGui::Button("Reload")) ConfigLoad();
            ImGui::SameLine();
            if (ImGui::Button("Apply in-game"))   // re-read skip lists + re-run autorun
            {
                RunCommand("reloadnetlog");
                RunCommand("runcfg");
                g_cfgStatus = "applied (reloadnetlog + runcfg) — Save first to persist";
            }
            ImGui::SameLine();
            ImGui::TextDisabled("edits config.yaml on disk (persists)");

            if (!g_cfgStatus.empty()) ImGui::TextDisabled("%s", g_cfgStatus.c_str());

            const float foot = ImGui::GetFrameHeightWithSpacing();
            ImGui::InputTextMultiline("##cfg", g_cfgText, sizeof(g_cfgText),
                                      ImVec2(-1.f, -foot), ImGuiInputTextFlags_AllowTabInput);
            ImGui::TextDisabled("Save writes to disk; Apply reloads it in-game. Full reload still needed for some settings.");
        }

        void DrawPanel()
        {
            if (ImGui::BeginTabBar("##octabs"))
            {
                if (ImGui::BeginTabItem("Console"))  { DrawConsoleTab();   ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Commands")) { DrawCommandsTab();  ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Vars"))     { DrawVarsTab();      ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Keybinds")) { DrawKeybindsTab();  ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Actors"))   { DrawActorsTab();    ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Config"))   { DrawConfigTab();    ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
        }
    } // anonymous namespace

    spdlog::sink_ptr GetSink()
    {
        if (!g_sink)
        {
            g_sink = std::make_shared<overlay_sink_mt>();
            // Time-only stamp (H:M:S.ms) — the date wastes horizontal space in the
            // overlay. Keeps logger name (%n) + level (%l): "[13:25:22.103] [drg] [info] msg".
            g_sink->set_pattern("[%H:%M:%S.%e] [%n] [%l] %v");
        }
        return g_sink;
    }

    void Init(CommandHandler* handler)
    {
        g_handler.store(handler);

        // Snapshot g_Vars on the game thread for the Vars tab (~2×/sec at 60fps).
        // Registered on EVERY Init: tick callbacks are cleared on UnloadMods, so a
        // `retry` would otherwise leave the Vars tab frozen. (The panel/tap below
        // are once-only because the overlay callback list survives the reload.)
        EnqueueEveryNTicks(30, []
        {
            std::vector<VarSnap> snap;
            snap.reserve(VarSystem::g_Vars.size());
            for (const auto& [name, v] : VarSystem::g_Vars)
                snap.push_back({ name, v.token, v.type });
            g_vars.store(std::move(snap));

            // Actor snapshot — walking GObjects is heavy, so only when explicitly
            // requested, or auto AND the Actors tab was rendered in the last ~700ms.
            if (g_actors.due(NowMs(), 700))
                g_actors.store(ActorList::Snapshot());
            return true;
        });

        // Register the panel exactly once per DLL lifetime. The overlay's
        // render-callback list outlives an in-process mod reload (`retry` →
        // UnloadMods/LoadMods → LoadModsGameThread → Init again); registering
        // each time would stack duplicate "Elytras" panels drawing the same
        // ImGui IDs → ID-conflict assert. (A full DLL reload resets this static.)
        static bool s_panelRegistered = false;
        if (s_panelRegistered) return;
        s_panelRegistered = true;

        // Mirror command responses (scan/list dumps, etc.) into the console pane —
        // they otherwise only travel over IPC to the CLI. Split on newlines so a
        // multi-line dump renders as individual, clippable log entries.
        g_responseTap = [](const std::string& msg)
        {
            PushLine(spdlog::level::info, msg);   // PushLine splits on newlines
        };

        // AddPanel's draw runs inside the overlay frame; set first-use size there.
        Overlay::AddPanel("Elytras", []
        {
            ImGui::SetWindowSize(ImVec2(680, 460), ImGuiCond_FirstUseEver);
            DrawPanel();
        }, ImGuiWindowFlags_NoCollapse);
    }
}
