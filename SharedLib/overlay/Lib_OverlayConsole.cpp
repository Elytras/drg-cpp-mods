// Lib_OverlayConsole.cpp — in-game CLI: log pane + console + command list.

#include "Lib_OverlayConsole.h"
#include "Lib_Overlay.h"
#include "Lib_CommandHandler.h"
#include "Lib_GameHooks.h"     // EnqueueOnce, EnqueueEveryNTicks
#include "Lib_VarSystem.h"     // VarSystem::Vars() store snapshot for the Vars tab
#include "Lib_KeyBindings.h"   // SnapshotCli for the Keybinds tab
#include "Lib_ActorList.h"     // actor snapshot for the Actors tab
#include "Lib_NetLogConfig.h"  // ConfigPath for the Config tab
#ifndef NOMINMAX
#define NOMINMAX               // StringLib pulls <Windows.h>; keep std::min/max usable
#endif
#include "StringLib.h"         // IEquals / IContains (canonical CI helpers)
#include "CoreUtils.h"         // SafeStof / SafeStoll (canonical parsers)
#include "GameThreadSnapshot.h" // double-buffered game-thread→UI snapshot
#include "OverlayTabs.h"        // detail:: shared services + tab registry (per-tab TUs)
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
#include <functional>

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

        // Shared services live in OverlayConsole::detail (defined at the bottom of
        // this TU, declared in OverlayTabs.h, and consumed by the per-tab TUs). Bring
        // the two the console/completion code reads unqualified into scope; the rest
        // are called qualified.
        using detail::RunCommand;
        using detail::VarSnap;

        // ── VarSystem snapshot (game thread writes, overlay reads) ───────────────
        // g_Vars is owned by the game thread; iterating it from the overlay thread
        // would race a concurrent insert/rehash. A periodic game-thread tick copies
        // it into g_vars; the Vars tab reads only this copy and pushes edits back as
        // `set <name> <value>` commands (game-thread safe). Exposed to the Vars/Actors
        // tab TUs via detail::Vars()/detail::Actors().
        GameThreadSnapshot<std::vector<VarSnap>> g_vars;   // refreshes unconditionally
        // Actors tab: game-thread-built snapshot. Heavy GObjects walk, so the snapshot
        // gates auto-refresh on a recent beat() (tab actually rendered) to avoid spiking
        // frametime when you aren't looking at the tab.
        GameThreadSnapshot<std::vector<ActorList::Row>> g_actors;

        // ── Console input + completion state (overlay thread only) ───────────────
        // Shared infra: read/written by the ImGui input callback, the completion
        // engine (ComputeCompletions/GhostSuffix/ArgPoolFor) and RunCommand — all
        // free functions because RunCommand is called by every tab and InputCallback
        // is a C-style ImGui callback. These move into the 3b TabContext, not into
        // ConsoleTab. (ConsoleTab owns only its private view state — see the class.)
        char        g_input[512]   = {};

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

        // ── Console tab — log pane + live filter + command input/autocomplete ───
        // Owns only its private view state; the input buffer, history, completion
        // candidates and RunCommand stay file-scope (shared input infra, see above).
        struct ConsoleTab : detail::OverlayTab<ConsoleTab>
        {
            static constexpr const char* kName = "Console";

            char logFilter[128] = {};   // log-pane live filter (substring, case-insensitive)
            bool autoScroll   = true;
            bool scrollBottom = false;
            bool focusInput   = false;  // re-grab input focus next frame (after a click)
            // Last g_logSeq the draw loop observed; auto-scroll pins only when this
            // changes (new lines arrived), so idle scroll-up works with it checked.
            uint64_t lastLogSeq = 0;
            // Pin-to-bottom countdown. GetScrollMaxY() lags one frame behind content
            // growth (ContentSize is finalized at End), so a single same-frame pin lands
            // a batch short; hold the pin a couple extra frames to settle on the bottom.
            int  pinFrames = 0;

            void Draw()
            {
            // Filter row (CLI Ctrl+F parity): live substring filter over the log pane.
            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputTextWithHint("##logfilter", "filter log (substring)...", logFilter, sizeof(logFilter));
            std::string lfilter = logFilter;

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
                const bool grew = g_logSeq != lastLogSeq;
                lastLogSeq = g_logSeq;
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
                if (scrollBottom || (autoScroll && grew)) pinFrames = 2;
                if (pinFrames > 0) { ImGui::SetScrollY(ImGui::GetScrollMaxY()); --pinFrames; }
                scrollBottom = false;
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
                            focusInput = true;   // jump back into the input box
                        }
                        x += (x > 0.f ? sp : 0.f) + iw;
                    }
                    ImGui::PopStyleColor();
                }
                ImGui::EndChild();
            }

            if (focusInput) { ImGui::SetKeyboardFocusHere(); focusInput = false; }
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
            ImGui::Checkbox("Auto-scroll", &autoScroll);

            if (enter || run)
            {
                RunCommand(g_input);
                g_input[0] = '\0';
                scrollBottom = true;
                ImGui::SetKeyboardFocusHere(-1);   // keep focus in the input box
            }
            }
        };

        // The other five tabs (Commands, Vars, Keybinds, Actors, Config) live in
        // overlay/tabs/*.cpp, each registered via its detail::RegisterXxxTab(). Only
        // ConsoleTab stays here — it owns the log ring buffer + completion engine.

        void DrawPanel()
        {
            if (ImGui::BeginTabBar("##octabs"))
            {
                for (auto& t : detail::Tabs())
                    if (ImGui::BeginTabItem(t.name)) { t.draw(); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
        }
    } // anonymous namespace

    // ── Shared services (declared in OverlayTabs.h, consumed by the tab TUs) ──────
    // Defined here, in the overlay core, because they wrap this TU's private state
    // (log ring buffer, command handler, snapshots, completion history). External
    // linkage so overlay/tabs/*.cpp can call them.
    namespace detail
    {

        //Only use this to run commands from the overlay. For some reason if you do it directly even through EnqueueOnce or other methods, you get disconnected as client, and i have no idea why.
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

        CommandHandler* Handler() { return g_handler.load(); }

        uint64_t NowMs()
        {
            using namespace std::chrono;
            return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        GameThreadSnapshot<std::vector<VarSnap>>&        Vars()   { return g_vars; }
        GameThreadSnapshot<std::vector<ActorList::Row>>& Actors() { return g_actors; }

        // Function-local static so the registry is alive before any tab self-registers,
        // independent of cross-TU static-init order.
        std::vector<OverlayTabEntry>& Tabs()
        {
            static std::vector<OverlayTabEntry> t;
            return t;
        }

        // ConsoleTab stays in this core TU (it owns the log/completion infra), so its
        // registrar lives here; the other tabs' registrars live in their own TUs.
        void RegisterConsoleTab() { static ConsoleTab s; (void)s; }
    }

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
            std::vector<detail::VarSnap> snap;
            snap.reserve(VarSystem::Vars().size());
            for (const auto& [name, v] : VarSystem::Vars())
            {
                if (v.flags & VarSystem::VarFlags::Hidden) continue;   // e.g. overlay.togglekey
                snap.push_back({ name, v.ToString(), v.Type() });
            }
            detail::Vars().store(std::move(snap));

            // Actor snapshot — walking GObjects is heavy, so only when explicitly
            // requested, or auto AND the Actors tab was rendered in the last ~700ms.
            if (detail::Actors().due(detail::NowMs(), 700))
                detail::Actors().store(ActorList::Snapshot());

#if defined(RogueCore) && RogueCore
            {
                static bool bInitialized = false;
                if (!bInitialized)
                {
                    std::vector<std::string> negotiations;
                        static constexpr std::array<std::string_view, 3> skipPrefixes{
                            "AWP_", "DECK_", "Default__"
                    };

                    for (auto* c : GObjectsOf<SDK::UBXEUnlockCollection>())
                    {
                        if (!c) continue;

                        std::string name = c->GetName();
                        if (StringLib::StartsWithAnyOf<char>(name, skipPrefixes))
                            continue;

                        negotiations.push_back(std::move(name));
                    }

                    std::sort(negotiations.begin(), negotiations.end());
                    bInitialized = !negotiations.empty();
                    detail::Negotiations().store(std::move(negotiations));
                }
            }
#endif
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

        // Build the tab registry once (overlay-session lifetime), in display order.
        // Each Register* call constructs that tab's session-lifetime instance, which
        // self-registers a {name, draw} entry via its OverlayTab<> base; DrawPanel
        // iterates detail::Tabs() in this call order. Registrars live in each tab's TU
        // (Console here in the core TU), so the order is fixed deterministically here
        // rather than by cross-TU static-init order.
        detail::RegisterConsoleTab();   // "Console"
        detail::RegisterCommandsTab();  // "Commands"
        detail::RegisterVarsTab();      // "Vars"
        detail::RegisterKeybindsTab();  // "Keybinds"
        detail::RegisterActorsTab();    // "Actors"

#if defined(RogueCore) && RogueCore
        detail::RegisterNegotiationTab();
        detail::RegisterResourceTab();
#endif
        detail::RegisterConfigTab();    // "Config" (last)

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
