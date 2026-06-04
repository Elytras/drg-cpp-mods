#pragma once
// WinInput.h — minimal Windows console readline with Tab-completion,
//               inline ghost-text hints, and up/down history.
//               No dependencies beyond Windows.h.
//
// Split-console mode:
//   Call SetSplitMode(mutex, logFn, resizeFn, getRowFn) to pin the input
//   strip to a fixed row. WinInput acquires *mutex during key dispatch,
//   uses logFn to emit completion candidates, and calls resizeFn on resize.
//   Viewport drag-resize is detected every event loop tick (cheap) — no
//   reliance on WINDOW_BUFFER_SIZE_EVENT alone.

#include <Windows.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <mutex>
#include <deque>

// ─────────────────────────────────────────────────────────────────────────────
//  Callback types
// ─────────────────────────────────────────────────────────────────────────────

using WinCompletionCallback = std::function<void(const std::string& buf, std::vector<std::string>& out)>;

struct WinHint
{
    std::string inlineText;  // dim completion text rendered over chars ahead of cursor
    std::string appendText;  // ghost text after end of buffer (params, owner, count)
    WORD        color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
};
using WinHintCallback        = std::function<WinHint(const std::string& buf, int cursor)>;
using WinDescriptionCallback = std::function<std::string(const std::string& candidate)>;

// ─────────────────────────────────────────────────────────────────────────────
//  WinInput
// ─────────────────────────────────────────────────────────────────────────────

class WinInput
{
    struct CandRegion { SHORT row, c0, c1; int idx; };

public:
    WinInput()
        : m_hIn(GetStdHandle(STD_INPUT_HANDLE))
        , m_hOut(GetStdHandle(STD_OUTPUT_HANDLE))
        , m_histMaxLen(200)
        , m_histIdx(-1)
        , m_row(0)
    {}

    void SetCompletionCallback(WinCompletionCallback cb)   { m_completionCb = std::move(cb); }
    void SetHintCallback(WinHintCallback cb)               { m_hintCb       = std::move(cb); }
    void SetDescriptionCallback(WinDescriptionCallback cb) { m_descCb       = std::move(cb); }
    void SetHistoryMaxLen(int n)                           { m_histMaxLen   = n; }
    void SetScrollFn(std::function<void(int)> fn)          { m_scrollFn     = std::move(fn); }

    // Called (while mutex is held) on every filter string change and on clear.
    void SetFilterCallbacks(std::function<void(const std::string&)> changeFn,
                            std::function<void()>                   clearFn)
    {
        m_filterChangeFn = std::move(changeFn);
        m_filterClearFn  = std::move(clearFn);
    }

    // Mouse-selection callbacks (all called while mutex is held).
    //   downFn(pos, rectMode) — LMB pressed; rectMode = Shift was held
    //   dragFn(pos)           — LMB held + mouse moved (or scroll-while-held)
    //   upFn(pos)             — LMB released
    //   copyFn()              — returns clipboard text for the current selection
    void SetMouseSelectionCallbacks(
        std::function<void(COORD, bool)> downFn,
        std::function<void(COORD)>       dragFn,
        std::function<void(COORD)>       upFn,
        std::function<std::string()>     copyFn)
    {
        m_mouseDownFn = std::move(downFn);
        m_mouseDragFn = std::move(dragFn);
        m_mouseUpFn   = std::move(upFn);
        m_copyFn      = std::move(copyFn);
    }

    // RMB clears the current selection (called while mutex is held).
    void SetClearSelectionCallback(std::function<void()> clearFn)
    {
        m_selClearFn = std::move(clearFn);
    }

    // Double-click in the log pane while a filter is active: drop the filter and
    // jump to the clicked line in the full log. Called while the mutex is held;
    // returns true iff a filter was active and the jump happened.
    void SetRevealFilteredLineCallback(std::function<bool(COORD)> fn)
    {
        m_revealFilteredLineFn = std::move(fn);
    }

    // Peek overlay: double-click a log line (no filter active) to open its full
    // text; the next keypress or mouse-click dismisses it. All called under mutex.
    //   peekFn   — open the peek for the clicked line; returns true iff opened
    //   closeFn  — close the peek if open; returns true iff one was active
    //   activeFn — query whether a peek is currently open
    void SetPeekCallbacks(std::function<bool(COORD)> peekFn,
                          std::function<bool()>      closeFn,
                          std::function<bool()>      activeFn)
    {
        m_peekFn       = std::move(peekFn);
        m_closePeekFn  = std::move(closeFn);
        m_peekActiveFn = std::move(activeFn);
    }

    // Enable dedicated AC pane mode.
    //   resizeFn — called (under mutex) with desired height; returns AC pane start row
    //   clearFn  — called (under mutex) when AC is dismissed
    void SetAcMode(std::function<SHORT(int)> resizeFn, std::function<void()> clearFn)
    {
        m_acResizeFn = std::move(resizeFn);
        m_acClearFn  = std::move(clearFn);
    }

    // Enable split-console mode.
    //   mutex     — held during all console writes (released while blocking on input)
    //   logFn     — called WHILE mutex is held; use *UnderLock variants
    //   resizeFn  — called while mutex is held on viewport resize; recalcs layout, returns new input row
    //   getRowFn  — called on every Redraw to read the current input row
    void SetSplitMode(std::mutex*                              mutex,
                      std::function<void(const std::string&)>  logFn,
                      std::function<SHORT()>                   resizeFn,
                      std::function<SHORT()>                   getRowFn)
    {
        m_splitMode     = true;
        m_splitMutex    = mutex;
        m_splitLogFn    = std::move(logFn);
        m_splitResizeFn = std::move(resizeFn);
        m_splitGetRowFn = std::move(getRowFn);
        m_splitInputRow = m_splitGetRowFn ? m_splitGetRowFn() : 0;
    }

    void HistoryAdd(const std::string& line)
    {
        if (!line.empty() && (m_history.empty() || m_history.front() != line))
            m_history.push_front(line);
        while ((int)m_history.size() > m_histMaxLen)
            m_history.pop_back();
    }

    bool Readline(const std::string& prompt, std::string& out)
    {
        DWORD prevMode = 0;
        GetConsoleMode(m_hIn, &prevMode);
        SetConsoleMode(m_hIn, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);

        m_buf.clear();
        m_cursor  = 0;
        m_histIdx = -1;
        m_histScratch.clear();

        if (m_splitMode)
            m_row = m_splitGetRowFn ? m_splitGetRowFn() : m_splitInputRow;
        else
        {
            CONSOLE_SCREEN_BUFFER_INFO csbi{};
            GetConsoleScreenBufferInfo(m_hOut, &csbi);
            m_row = csbi.dwCursorPosition.Y;
        }

        // Snapshot viewport size for drag-resize detection
        {
            CONSOLE_SCREEN_BUFFER_INFO csbi{};
            GetConsoleScreenBufferInfo(m_hOut, &csbi);
            m_viewW = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
            m_viewH = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
        }

        { auto lock = MakeLock(); Redraw(prompt); }

        bool result = true;

        while (true)
        {
            // Wait with a 150 ms timeout so viewport drag-resize is detected
            // even when no input events fire (viewport-only resize doesn't
            // generate WINDOW_BUFFER_SIZE_EVENT in all console hosts).
            if (WaitForSingleObject(m_hIn, 150) == WAIT_TIMEOUT)
            {
                CheckAndHandleResize(prompt);
                continue;
            }

            INPUT_RECORD rec{};
            DWORD nRead = 0;
            ReadConsoleInputW(m_hIn, &rec, 1, &nRead);
            if (nRead == 0) continue;

            // Also check on every arriving event for immediate response
            if (CheckAndHandleResize(prompt)) continue;

            // ── Mouse events ──────────────────────────────────────────────
            if (rec.EventType == MOUSE_EVENT)
            {
                const MOUSE_EVENT_RECORD& me     = rec.Event.MouseEvent;
                const COORD               mpos   = me.dwMousePosition;
                const bool                lmbNow = (me.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;

                m_lastMousePos = mpos;

                // ── Peek overlay active — swallow mouse input ─────────────────
                // A button press or wheel dismisses the peek; pure moves are
                // ignored so the frozen overlay doesn't react to the cursor.
                if (m_peekActiveFn && m_peekActiveFn())
                {
                    const bool acted = (me.dwButtonState != 0) ||
                                       (me.dwEventFlags & MOUSE_WHEELED);
                    if (acted && m_closePeekFn)
                    {
                        auto lock = MakeLock();
                        m_closePeekFn();
                        RedrawActive(prompt);
                    }
                    continue;
                }

                // ── Double-click — reveal a filtered line in the full log ─────
                // Fires only on a genuine OS double-click (not a select-then-click),
                // so an existing drag-selection can't be mistaken for this gesture.
                // The leading single click of the double starts a 1-line selection;
                // RevealFilteredLineUnderLock clears it and we drop m_lmbHeld here.
                if (me.dwEventFlags & DOUBLE_CLICK)
                {
                    if (m_splitMode && m_revealFilteredLineFn)
                    {
                        auto lock = MakeLock();
                        if (m_revealFilteredLineFn(mpos))
                        {
                            m_lmbHeld = false;   // cancel the nascent 1-click selection
                            // SplitConsole already dropped its filter; sync our own
                            // filter-edit state without re-invoking the clear callback.
                            if (m_filterMode)
                            {
                                m_buf        = m_savedBuf;
                                m_cursor     = m_savedCursor;
                                m_filterMode = false;
                                m_filterBuf.clear();
                                Redraw(m_savedPrompt);
                            }
                            else
                            {
                                m_filterBuf.clear();
                                Redraw(prompt);
                            }
                            continue;
                        }
                    }
                    // No filter to reveal — open the full-text peek overlay for the
                    // clicked line so over-long entries become readable.
                    if (m_splitMode && m_peekFn)
                    {
                        auto lock = MakeLock();
                        if (m_peekFn(mpos))
                        {
                            m_lmbHeld = false;   // cancel the nascent 1-click selection
                            RedrawActive(prompt);
                        }
                    }
                    continue;
                }

                // ── Wheel scroll ──────────────────────────────────────────
                if (me.dwEventFlags & MOUSE_WHEELED)
                {
                    SHORT delta = (SHORT)HIWORD(me.dwButtonState);
                    if (m_splitMode && m_scrollFn)
                    {
                        auto lock = MakeLock();
                        m_scrollFn((delta > 0) ? +3 : -3);
                        // Keep selection endpoint at current mouse position
                        // so the selection extends into the newly scrolled view.
                        if (m_lmbHeld && m_mouseDragFn) m_mouseDragFn(m_lastMousePos);
                        RedrawActive(prompt);
                    }
                    else
                    {
                        int scroll = (delta > 0) ? -3 : 3;
                        CONSOLE_SCREEN_BUFFER_INFO csbi{};
                        GetConsoleScreenBufferInfo(m_hOut, &csbi);
                        SMALL_RECT win = csbi.srWindow;
                        if (scroll < 0 && win.Top  + scroll < 0)                scroll = -win.Top;
                        if (scroll > 0 && win.Bottom + scroll >= csbi.dwSize.Y) scroll = csbi.dwSize.Y - 1 - win.Bottom;
                        if (scroll != 0)
                        {
                            win.Top    += (SHORT)scroll;
                            win.Bottom += (SHORT)scroll;
                            SetConsoleWindowInfo(m_hOut, TRUE, &win);
                        }
                    }
                    continue;
                }

                // ── Mouse move ────────────────────────────────────────────
                if (me.dwEventFlags & MOUSE_MOVED)
                {
                    // AC candidate hover (existing behaviour).
                    if (!m_candRegs.empty())
                    {
                        int newHovered = -1;
                        for (const auto& cr : m_candRegs)
                        {
                            if (mpos.Y == cr.row && mpos.X >= cr.c0 && mpos.X < cr.c1)
                            { newHovered = cr.idx; break; }
                        }
                        if (newHovered != m_hoveredCand)
                        {
                            auto lock = MakeLock();
                            int oldHovered = m_hoveredCand;
                            m_hoveredCand  = newHovered;
                            HighlightCandidate(oldHovered, false);
                            HighlightCandidate(newHovered, true);
                            if (newHovered >= 0 && m_descCb)
                                ShowTooltip(m_descCb(m_lastCandidates[newHovered]));
                            else
                                ClearTooltip();
                            Redraw(m_prompt);
                        }
                    }
                    // Selection drag — only while LMB is held and not over AC pane.
                    if (lmbNow && m_lmbHeld && m_mouseDragFn && m_candRegs.empty())
                    {
                        auto lock = MakeLock();
                        m_mouseDragFn(mpos);
                        RedrawActive(prompt);
                    }
                    continue;
                }

                // ── Button state change ───────────────────────────────────
                if (!me.dwEventFlags)
                {
                    // RMB pressed — clear selection.
                    const bool rmbNow = (me.dwButtonState & RIGHTMOST_BUTTON_PRESSED) != 0;
                    if (rmbNow && m_selClearFn)
                    {
                        auto lock = MakeLock();
                        m_selClearFn();
                        RedrawActive(prompt);
                    }

                    if (lmbNow && !m_lmbHeld)
                    {
                        // LMB just pressed.
                        m_lmbHeld = true;
                        const bool shift = (me.dwControlKeyState & SHIFT_PRESSED) != 0;

                        // AC candidate click takes priority.
                        bool clickedCand = false;
                        if (!m_candRegs.empty())
                        {
                            for (const auto& cr : m_candRegs)
                            {
                                if (mpos.Y == cr.row && mpos.X >= cr.c0 && mpos.X < cr.c1)
                                {
                                    auto lock = MakeLock();
                                    m_buf    = m_lastCandidates[cr.idx];
                                    m_cursor = (int)m_buf.size();
                                    ClearCandidateState();
                                    RedrawActive(prompt);
                                    clickedCand = true;
                                    break;
                                }
                            }
                        }

                        if (!clickedCand && m_mouseDownFn)
                        {
                            auto lock = MakeLock();
                            ClearCandidateState();  // dismiss AC if open
                            m_mouseDownFn(mpos, shift);
                            RedrawActive(prompt);
                        }
                    }
                    else if (!lmbNow && m_lmbHeld)
                    {
                        // LMB just released.
                        m_lmbHeld = false;
                        if (m_mouseUpFn)
                        {
                            auto lock = MakeLock();
                            m_mouseUpFn(mpos);
                            RedrawActive(prompt);
                        }
                    }
                }
                continue;
            }

            if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
                continue;

            // Hold the mutex for the entire key-dispatch + redraw
            auto lock = MakeLock();

            const KEY_EVENT_RECORD& ke = rec.Event.KeyEvent;
            WORD    vk = ke.wVirtualKeyCode;
            wchar_t ch = ke.uChar.UnicodeChar;

            // Ignore pure modifier / non-editor keys entirely so they don't
            // dismiss the AC pane or ghost text (e.g. Win+Shift+S for screenshot).
            if (IsIgnoredKey(vk)) continue;

            // While a log line is being peeked, the next keystroke dismisses the
            // overlay and is otherwise swallowed (so Esc/Enter/arrows just close).
            if (m_closePeekFn && m_closePeekFn()) { RedrawActive(prompt); continue; }

            // Tab re-enters DoCompletion which manages its own AC state.
            // Any other key dismisses the AC pane and clears tracking state.
            // If the AC pane was active, ClearCandidateState triggers RenderAll
            // in SplitConsole which blanks the input row — redraw immediately so
            // the prompt is restored before the key handler runs.
            if (vk != VK_TAB)
            {
                bool acWasActive = (m_acAreaStartRow >= 0);
                ClearCandidateState();
                if (acWasActive) RedrawActive(prompt);
            }

            // ── Ctrl+F — toggle filter mode ───────────────────────────────
            if (ch == 0x06)
            {
                if (!m_filterMode)
                {
                    // Enter filter mode: swap command buf for filter buf.
                    m_savedBuf    = m_buf;
                    m_savedCursor = m_cursor;
                    m_savedPrompt = prompt;
                    m_buf         = m_filterBuf;
                    m_cursor      = (int)m_filterBuf.size();
                    m_filterMode  = true;
                    Redraw(kFilterPrompt);
                }
                else
                {
                    // Exit filter mode: keep filter active, restore command buf.
                    m_filterBuf  = m_buf;
                    m_buf        = m_savedBuf;
                    m_cursor     = m_savedCursor;
                    m_filterMode = false;
                    if (m_filterChangeFn) m_filterChangeFn(m_filterBuf);
                    Redraw(m_savedPrompt);
                }
                continue;
            }

            // ── Ctrl+C — copy selection to clipboard ──────────────────────
            if (ch == 0x03)
            {
                if (m_copyFn)
                {
                    std::string text = m_copyFn();
                    if (!text.empty())
                    {
                        SetClipboard(text);
                        int lines = (int)std::count(text.begin(), text.end(), '\n');
                        if (m_splitLogFn)
                            m_splitLogFn("[Copied " + std::to_string(lines) + " line(s) to clipboard]");
                    }
                }
                continue;
            }

            // ── Ctrl+V — paste clipboard text (command & filter modes) ───
            if (ch == 0x16)
            {
                std::string paste = SanitizePaste(GetClipboardText());
                if (!paste.empty())
                {
                    m_buf.insert(m_cursor, paste);
                    m_cursor += (int)paste.size();
                    RedrawActive(prompt);
                    if (m_filterMode && m_filterChangeFn) m_filterChangeFn(m_buf);
                }
                continue;
            }

            if (vk == VK_RETURN)
            {
                // Shift+Enter inserts a literal newline instead of submitting
                // (command/typing mode only — a multi-line filter string is
                // meaningless against single-line log entries).
                const bool shiftEnter = (ke.dwControlKeyState & SHIFT_PRESSED) != 0;
                if (shiftEnter && !m_filterMode)
                {
                    m_buf.insert(m_cursor, "\n", 1);
                    m_cursor += 1;
                    Redraw(prompt);
                    continue;
                }
                if (m_filterMode)
                {
                    // Commit filter, return to command mode (filter stays active).
                    m_filterBuf  = m_buf;
                    m_buf        = m_savedBuf;
                    m_cursor     = m_savedCursor;
                    m_filterMode = false;
                    if (m_filterChangeFn) m_filterChangeFn(m_filterBuf);
                    Redraw(m_savedPrompt);
                    continue;
                }
                if (m_splitMode)
                {
                    CONSOLE_SCREEN_BUFFER_INFO csbi{};
                    GetConsoleScreenBufferInfo(m_hOut, &csbi);
                    COORD pos{0, m_row};
                    DWORD written;
                    FillConsoleOutputCharacterW(m_hOut, L' ', csbi.dwSize.X, pos, &written);
                    SetConsoleCursorPosition(m_hOut, pos);
                }
                else
                {
                    CONSOLE_SCREEN_BUFFER_INFO csbi{};
                    GetConsoleScreenBufferInfo(m_hOut, &csbi);
                    SetConsoleCursorPosition(m_hOut, {0, csbi.dwCursorPosition.Y});
                    WriteConsoleW(m_hOut, L"\r\n", 2, nullptr, nullptr);
                }
                break;
            }

            if (vk == VK_ESCAPE)
            {
                if (m_filterMode)
                {
                    // Clear filter entirely, return to command mode.
                    m_filterBuf.clear();
                    m_buf        = m_savedBuf;
                    m_cursor     = m_savedCursor;
                    m_filterMode = false;
                    if (m_filterClearFn) m_filterClearFn();
                    Redraw(m_savedPrompt);
                    continue;
                }
                m_buf.clear(); m_cursor = 0;
                Redraw(prompt);
                continue;
            }

            if (vk == VK_BACK || ch == 0x08 || ch == 0x7F)
            {
                bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                // ch == 0x7F is the DEL char that Windows consoles emit for
                // Ctrl+Backspace even when Ctrl isn't reported in dwControlKeyState.
                if (ctrl || ch == 0x7F)
                {
                    // Word-erase. Matches Ctrl+Left's boundary logic:
                    //   1) skip whitespace going left
                    //   2) skip non-whitespace going left
                    // Effect: inside a word → removes left half of that word.
                    //         at start of word / in whitespace → removes the
                    //         previous word plus the whitespace before it.
                    int c = m_cursor;
                    while (c > 0 && m_buf[c - 1] == ' ') --c;
                    while (c > 0 && m_buf[c - 1] != ' ') --c;
                    if (c != m_cursor)
                    {
                        m_buf.erase(c, m_cursor - c);
                        m_cursor = c;
                        RedrawActive(prompt);
                        if (m_filterMode && m_filterChangeFn) m_filterChangeFn(m_buf);
                    }
                }
                else if (m_cursor > 0)
                {
                    m_buf.erase(m_cursor - 1, 1);
                    --m_cursor;
                    RedrawActive(prompt);
                    if (m_filterMode && m_filterChangeFn) m_filterChangeFn(m_buf);
                }
                continue;
            }

            if (vk == VK_DELETE)
            {
                if (m_cursor < (int)m_buf.size())
                {
                    m_buf.erase(m_cursor, 1);
                    RedrawActive(prompt);
                    if (m_filterMode && m_filterChangeFn) m_filterChangeFn(m_buf);
                }
                continue;
            }

            if (vk == VK_LEFT)
            {
                bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                if (ctrl)
                {
                    int c = m_cursor;
                    while (c > 0 && m_buf[c - 1] == ' ') --c;
                    while (c > 0 && m_buf[c - 1] != ' ') --c;
                    if (c != m_cursor) { m_cursor = c; RedrawActive(prompt); }
                }
                else if (m_cursor > 0) { --m_cursor; RedrawActive(prompt); }
                continue;
            }

            if (vk == VK_RIGHT)
            {
                bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                if (ctrl)
                {
                    int c = m_cursor, n = (int)m_buf.size();
                    while (c < n && m_buf[c] != ' ') ++c;
                    while (c < n && m_buf[c] == ' ') ++c;
                    if (c != m_cursor) { m_cursor = c; RedrawActive(prompt); }
                }
                else if (m_cursor < (int)m_buf.size())
                {
                    ++m_cursor;
                    RedrawActive(prompt);
                }
                else if (!m_filterMode)
                {
                    // At end of buffer: accept inlineText ghost completion
                    // (ghost completions are not relevant in filter mode)
                    WinHint hint = Hint();
                    if (!hint.inlineText.empty())
                    {
                        m_cursor = std::min(m_cursor + (int)hint.inlineText.size(),
                                            (int)m_buf.size());
                        Redraw(prompt);
                    }
                }
                continue;
            }

            // No-redraw guards: skip Redraw when cursor is already at the target position
            if (vk == VK_HOME)
            {
                if (m_filterMode)
                {
                    // In filter mode HOME moves the text cursor (no log scroll).
                    if (m_cursor != 0) { m_cursor = 0; Redraw(kFilterPrompt); }
                }
                // Split mode: jump to oldest log entry (scroll offset = max).
                // Otherwise: move text cursor to start of input buffer.
                else if (m_splitMode && m_scrollFn) { m_scrollFn(INT_MAX);  Redraw(prompt); }
                else if (m_cursor != 0)             { m_cursor = 0;         Redraw(prompt); }
                continue;
            }
            if (vk == VK_END)
            {
                if (m_filterMode)
                {
                    // In filter mode END moves the text cursor (no log scroll).
                    int e = (int)m_buf.size();
                    if (m_cursor != e) { m_cursor = e; Redraw(kFilterPrompt); }
                }
                // Split mode: jump to newest log entry (scroll offset = 0).
                // Otherwise: move text cursor to end of input buffer.
                else if (m_splitMode && m_scrollFn)              { m_scrollFn(-INT_MAX); Redraw(prompt); }
                else { int e=(int)m_buf.size(); if (m_cursor != e) { m_cursor = e;  Redraw(prompt); } }
                continue;
            }

            if (vk == VK_PRIOR || vk == VK_NEXT)  // PgUp / PgDn — scroll log pane
            {
                if (m_splitMode && m_scrollFn)
                {
                    CONSOLE_SCREEN_BUFFER_INFO csbi2{};
                    GetConsoleScreenBufferInfo(m_hOut, &csbi2);
                    int pageH = std::max(1, (int)(csbi2.srWindow.Bottom - csbi2.srWindow.Top + 1) / 2);
                    m_scrollFn(vk == VK_PRIOR ? +pageH : -pageH);
                    RedrawActive(prompt);
                }
                continue;
            }

            if (vk == VK_UP)
            {
                if (m_filterMode)
                {
                    if (m_splitMode && m_scrollFn) { m_scrollFn(+1); Redraw(kFilterPrompt); }
                    continue;
                }
                NavigateHistory(prompt, +1);
                continue;
            }
            if (vk == VK_DOWN)
            {
                if (m_filterMode)
                {
                    if (m_splitMode && m_scrollFn) { m_scrollFn(-1); Redraw(kFilterPrompt); }
                    continue;
                }
                NavigateHistory(prompt, -1);
                continue;
            }

            // Ctrl-A / Ctrl-E / Ctrl-K / Ctrl-U with no-redraw guards
            if (ch == 0x01) { if (m_cursor != 0)                { m_cursor = 0;          RedrawActive(prompt); } continue; }
            if (ch == 0x05) { int e=(int)m_buf.size(); if (m_cursor != e) { m_cursor = e; RedrawActive(prompt); } continue; }
            if (ch == 0x0B) { if (m_cursor < (int)m_buf.size()) { m_buf.erase(m_cursor); RedrawActive(prompt);
                                  if (m_filterMode && m_filterChangeFn) m_filterChangeFn(m_buf); } continue; }
            if (ch == 0x15) { m_buf.erase(0, m_cursor); m_cursor = 0;                    RedrawActive(prompt);
                              if (m_filterMode && m_filterChangeFn) m_filterChangeFn(m_buf); continue; }

            // Ctrl-D on empty line → EOF (treat as Escape in filter mode to avoid
            // accidentally terminating the REPL while editing a filter string)
            if (ch == 0x04 && m_buf.empty())
            {
                if (m_filterMode)
                {
                    m_filterBuf.clear();
                    m_buf        = m_savedBuf;
                    m_cursor     = m_savedCursor;
                    m_filterMode = false;
                    if (m_filterClearFn) m_filterClearFn();
                    Redraw(m_savedPrompt);
                    continue;
                }
                WriteConsoleW(m_hOut, L"\r\n", 2, nullptr, nullptr);
                result = false;
                break;
            }

            if (vk == VK_TAB)
            {
                if (m_filterMode) continue;  // no-op: completions don't apply to filters
                if (m_hoveredCand >= 0 && m_hoveredCand < (int)m_lastCandidates.size())
                {
                    m_buf    = m_lastCandidates[m_hoveredCand];
                    m_cursor = (int)m_buf.size();
                    ClearCandidateState();
                    Redraw(prompt);
                }
                else
                {
                    DoCompletion(prompt);
                }
                continue;
            }

            if (ch >= 0x20 && ch != 0x7F)
            {
                char narrow[4]{};
                int bytes = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, narrow, sizeof(narrow), nullptr, nullptr);
                if (bytes > 0)
                {
                    m_buf.insert(m_cursor, narrow, bytes);
                    m_cursor += bytes;
                    RedrawActive(prompt);
                    if (m_filterMode && m_filterChangeFn) m_filterChangeFn(m_buf);
                }
                continue;
            }
        }

        SetConsoleMode(m_hIn, prevMode);
        out = m_buf;
        return result;
    }

private:
    // ── Modifier-key filter ───────────────────────────────────────────────
    // Returns true for keys that carry no semantic meaning for the editor
    // (pure modifiers, dead keys, media keys, etc.) and should be ignored
    // completely — no state change, no AC pane dismissal.
    static bool IsIgnoredKey(WORD vk)
    {
        switch (vk)
        {
        case VK_SHIFT:    case VK_LSHIFT:   case VK_RSHIFT:
        case VK_CONTROL:  case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU:     case VK_LMENU:    case VK_RMENU:
        case VK_LWIN:     case VK_RWIN:     case VK_APPS:
        case VK_CAPITAL:  case VK_NUMLOCK:  case VK_SCROLL:
        case VK_SNAPSHOT: case VK_PAUSE:    case VK_INSERT:
        case VK_F1:  case VK_F2:  case VK_F3:  case VK_F4:
        case VK_F5:  case VK_F6:  case VK_F7:  case VK_F8:
        case VK_F9:  case VK_F10: case VK_F11: case VK_F12:
            return true;
        default:
            return false;
        }
    }

    // ── Resize helper ─────────────────────────────────────────────────────
    // Checks whether the console viewport changed; if so, triggers a full
    // layout recalc + redraw. Returns true if a resize was handled.
    bool CheckAndHandleResize(const std::string& prompt)
    {
        if (!m_splitMode || !m_splitResizeFn) return false;
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        SHORT newW = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        SHORT newH = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
        if (newW == m_viewW && newH == m_viewH) return false;
        m_viewW = newW;
        m_viewH = newH;
        // Drain any queued WINDOW_BUFFER_SIZE_EVENTs to avoid re-entry
        INPUT_RECORD drain{}; DWORD nDrain = 0;
        while (PeekConsoleInputW(m_hIn, &drain, 1, &nDrain) && nDrain > 0
               && drain.EventType == WINDOW_BUFFER_SIZE_EVENT)
            ReadConsoleInputW(m_hIn, &drain, 1, &nDrain);
        auto lock = MakeLock();
        m_splitInputRow = m_splitResizeFn();
        m_row = m_splitInputRow;
        RedrawActive(prompt);
        return true;
    }

    // ── Split-mode lock helper ─────────────────────────────────────────────
    std::unique_lock<std::mutex> MakeLock()
    {
        if (m_splitMutex)
            return std::unique_lock<std::mutex>(*m_splitMutex);
        return {};
    }

    // ── Low-level helpers ──────────────────────────────────────────────────

    // UTF-8 → wide for display, with control chars mapped to fixed-width glyphs so
    // a newline (Shift+Enter / pasted) or stray control byte can't break the
    // single-row input layout. One source-of-truth used by both WriteNarrow and
    // ColWidth, so rendered width and cursor-column math always agree.
    static std::wstring DisplayWide(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        if (n <= 0) return {};
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
        for (wchar_t& c : w)
        {
            if (c == L'\n')    c = L'\x21B5';  // ↵ newline marker
            else if (c < 0x20) c = L'\xB7';    // · any other control char (tab, etc.)
        }
        return w;
    }

    void WriteNarrow(const std::string& s)
    {
        if (s.empty()) return;
        std::wstring w = DisplayWide(s);
        DWORD written;
        WriteConsoleW(m_hOut, w.data(), (int)w.size(), &written, nullptr);
    }

    int ColWidth(const std::string& s)
    {
        return (int)DisplayWide(s).size();
    }

    // ── Hint ──────────────────────────────────────────────────────────────

    WinHint Hint()
    {
        // No autocomplete hints while editing a filter — completions don't apply
        // to filter strings, so suppress both the rendered ghost text and any
        // End-key accept path that reads this.
        if (m_filterMode) return {};
        // When a candidate is hovered, use it as the active completion
        // regardless of what the regular hint callback would return.
        if (m_hoveredCand >= 0 && m_hoveredCand < (int)m_lastCandidates.size())
            return HoverHint(m_lastCandidates[m_hoveredCand]);
        if (!m_hintCb) return {};
        return m_hintCb(m_buf, m_cursor);
    }

    // Build a ghost-text hint for a specific candidate string.
    // inlineText = the part of the candidate that comes after the cursor position.
    // appendText = description from m_descCb (if any).
    WinHint HoverHint(const std::string& candidate)
    {
        WinHint h;
        h.color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; // dim gray

        // Chars of candidate beyond current cursor position → ghost text
        if ((size_t)m_cursor < candidate.size())
        {
            std::string remainder = candidate.substr(m_cursor);
            int ahead = (int)m_buf.size() - m_cursor;
            if (ahead <= 0)
            {
                h.inlineText = remainder;
            }
            else
            {
                // Cursor is mid-buffer: dim only the chars that match ahead
                int overlap = std::min((int)remainder.size(), ahead);
                bool ok = true;
                for (int i = 0; i < overlap && ok; ++i)
                    if (tolower((unsigned char)m_buf[m_cursor + i]) !=
                        tolower((unsigned char)remainder[i])) ok = false;
                if (ok) h.inlineText = remainder.substr(0, overlap);
            }
        }

        if (m_descCb)
        {
            std::string desc = m_descCb(candidate);
            if (!desc.empty()) h.appendText = "  " + desc;
        }
        return h;
    }

    // Change the console color attribute for one candidate cell.
    // highlight=true → selection color; false → restore default.
    void HighlightCandidate(int candIdx, bool highlight)
    {
        if (candIdx < 0) return;
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        // Selection: bright white on blue; matches classic console "selected" look
        constexpr WORD kSelAttr = BACKGROUND_BLUE
                                | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE
                                | FOREGROUND_INTENSITY;
        WORD attr = highlight ? kSelAttr : csbi.wAttributes;
        for (const auto& cr : m_candRegs)
        {
            if (cr.idx != candIdx) continue;
            int w = cr.c1 - cr.c0;
            if (w <= 0) continue;
            DWORD written;
            FillConsoleOutputAttribute(m_hOut, attr, w, {cr.c0, cr.row}, &written);
        }
    }

    // ── Redraw ────────────────────────────────────────────────────────────

    static constexpr const char* kFilterPrompt = "[Filter]> ";

    // Copy UTF-8 text to the Windows clipboard as CF_UNICODETEXT.
    static void SetClipboard(const std::string& utf8)
    {
        if (utf8.empty()) return;
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (n <= 0) return;
        std::wstring wide(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), n);
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        if (HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t)))
        {
            memcpy(GlobalLock(hMem), wide.data(), (size_t)n * sizeof(wchar_t));
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
    }

    // Read CF_UNICODETEXT from the clipboard as UTF-8 ("" if none/!text).
    static std::string GetClipboardText()
    {
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return {};
        if (!OpenClipboard(nullptr)) return {};
        std::string out;
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        {
            if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h)))
            {
                int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                if (n > 0)
                {
                    out.resize(n);
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
                    if (!out.empty() && out.back() == '\0') out.pop_back();  // drop NUL terminator
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return out;
    }

    // Make pasted text safe for the single-line buffer: drop NUL bytes entirely
    // and normalise CRLF / lone CR to a single LF (kept as-is in the buffer and
    // rendered via DisplayWide's ↵ marker).
    static std::string SanitizePaste(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            const char c = in[i];
            if (c == '\0') continue;
            if (c == '\r')
            {
                out.push_back('\n');
                if (i + 1 < in.size() && in[i + 1] == '\n') ++i;  // collapse CRLF
                continue;
            }
            out.push_back(c);
        }
        return out;
    }

    // Use instead of Redraw(prompt) for key handlers that run in filter mode:
    // picks the filter prompt when in filter mode, command prompt otherwise.
    void RedrawActive(const std::string& cmdPrompt)
    {
        Redraw(m_filterMode ? std::string(kFilterPrompt) : cmdPrompt);
    }

    void Redraw(const std::string& prompt)
    {
        m_prompt = prompt;  // store for ShowTooltip / ClearTooltip

        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        SHORT bufW = csbi.dwSize.X;

        if (m_splitMode)
            m_row = m_splitGetRowFn ? m_splitGetRowFn() : m_splitInputRow;
        else
        {
            SHORT curRow = csbi.dwCursorPosition.Y;
            if (curRow < m_row) m_row = curRow;
            if (m_row < 0) m_row = 0;
            if (m_row >= csbi.dwSize.Y) m_row = csbi.dwSize.Y - 1;
        }

        COORD home{ 0, m_row };
        DWORD written;
        FillConsoleOutputCharacterW(m_hOut, L' ', bufW, home, &written);
        FillConsoleOutputAttribute(m_hOut, csbi.wAttributes, bufW, home, &written);
        SetConsoleCursorPosition(m_hOut, home);

        WinHint hint = Hint();

        // Segment 1: prompt
        WriteNarrow(prompt);

        // Segment 2: buf[0..cursor] in normal color
        if (m_cursor > 0)
            WriteNarrow(m_buf.substr(0, m_cursor));

        // Segment 3: inlineText (dim completion ahead of cursor)
        if (!hint.inlineText.empty())
        {
            SetConsoleTextAttribute(m_hOut, hint.color);
            WriteNarrow(hint.inlineText);
            SetConsoleTextAttribute(m_hOut, csbi.wAttributes);
        }

        // Segment 4: buf[cursor+inlineLen..end] in normal color
        {
            int afterInline = m_cursor + (int)hint.inlineText.size();
            if (afterInline < (int)m_buf.size())
                WriteNarrow(m_buf.substr(afterInline));
        }

        // Segment 5: appendText — truncated so it never wraps
        if (!hint.appendText.empty())
        {
            int used      = ColWidth(prompt) + ColWidth(m_buf) + ColWidth(hint.inlineText);
            int available = (int)bufW - used - 1;
            std::string appendToShow = hint.appendText;
            if (available <= 0)
                appendToShow.clear();
            else if (ColWidth(appendToShow) > available)
                appendToShow.resize((size_t)available);
            if (!appendToShow.empty())
            {
                SetConsoleTextAttribute(m_hOut, hint.color);
                WriteNarrow(appendToShow);
                SetConsoleTextAttribute(m_hOut, csbi.wAttributes);
            }
        }

        int col = ColWidth(prompt) + ColWidth(m_buf.substr(0, m_cursor));
        if (col < 0) col = 0;
        if (col >= bufW) col = bufW - 1;
        SetConsoleCursorPosition(m_hOut, { (SHORT)col, m_row });
    }

    // ── History ───────────────────────────────────────────────────────────

    void NavigateHistory(const std::string& prompt, int dir)
    {
        if (m_history.empty()) return;
        if (m_histIdx == -1) m_histScratch = m_buf;

        m_histIdx += dir;
        if (m_histIdx < 0)
        {
            m_histIdx = -1;
            m_buf = m_histScratch;
        }
        else if (m_histIdx >= (int)m_history.size())
        {
            m_histIdx = (int)m_history.size() - 1;
        }
        else
        {
            m_buf = m_history[m_histIdx];
        }

        m_cursor = (int)m_buf.size();
        Redraw(prompt);
    }

    // ── WriteLineAt helper — write a UTF-8 string directly to a console row ──

    void WriteLineAt(SHORT row, const std::string& utf8)
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        SHORT w = csbi.dwSize.X;
        std::wstring wline;
        if (!utf8.empty())
        {
            int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
            if (n > 0) { wline.resize(n); MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wline.data(), n); }
        }
        if ((int)wline.size() > w) wline.resize(w);
        COORD pos{0, row};
        DWORD written;
        FillConsoleOutputCharacterW(m_hOut, L' ', w, pos, &written);
        FillConsoleOutputAttribute(m_hOut, csbi.wAttributes, w, pos, &written);
        if (!wline.empty())
            WriteConsoleOutputCharacterW(m_hOut, wline.c_str(), (DWORD)wline.size(), pos, &written);
    }

    // ── Tab completion ─────────────────────────────────────────────────────

    void DoCompletion(const std::string& prompt)
    {
        ClearTooltip(); // clear tooltip without collapsing AC pane

        if (!m_completionCb) return;

        std::vector<std::string> candidates;
        m_completionCb(m_buf, candidates);

        auto dismissAc = [&]()
        {
            if (m_acClearFn) m_acClearFn();
            m_acAreaStartRow = -1;
            m_candRegs.clear();
            m_lastCandidates.clear();
            m_hoveredCand = -1;
            m_candBaseRow = -1;
        };

        if (candidates.empty())
        {
            dismissAc();
            return;
        }

        if (candidates.size() == 1)
        {
            m_buf    = candidates[0];
            m_cursor = (int)m_buf.size();
            dismissAc();
            Redraw(prompt);
            return;
        }

        // Extend to longest common prefix
        std::string lcp = candidates[0];
        for (size_t i = 1; i < candidates.size(); ++i)
        {
            size_t j = 0;
            while (j < lcp.size() && j < candidates[i].size() && lcp[j] == candidates[i][j]) ++j;
            lcp = lcp.substr(0, j);
        }
        if (lcp.size() > m_buf.size())
        {
            m_buf    = lcp;
            m_cursor = (int)m_buf.size();
            dismissAc();
            Redraw(prompt);
            return;
        }

        // Multiple candidates — format them into columns
        CONSOLE_SCREEN_BUFFER_INFO csbiC{};
        GetConsoleScreenBufferInfo(m_hOut, &csbiC);
        int candBufW = csbiC.dwSize.X;
        int maxCandW = 0;
        for (const auto& c : candidates)
            maxCandW = std::max(maxCandW, ColWidth(c));
        int colW     = maxCandW + 2;
        int numCols  = std::max(1, candBufW / colW);
        int numCandRows = ((int)candidates.size() + numCols - 1) / numCols;

        m_hoveredCand = -1;

        if (m_acResizeFn)
        {
            // Dedicated AC pane mode.
            // Row 0 of the AC pane is the tooltip row; candidates start at row 1.
            // Budget: keep the input row + CLI output pane (~3) + 2 dividers +
            // ~3 minimum log rows visible, and hand the rest to candidates.
            // The previous `consoleH/2 - 1` cap meant `call <Tab>` could only
            // show ~14 rows even on a 40-row terminal — way too few once
            // `scanfuncs` returns 600+ matches.
            const int consoleH = csbiC.srWindow.Bottom - csbiC.srWindow.Top + 1;
            constexpr int kReserve = /*CLI pane*/3 + /*dividers*/2 + /*min log*/3;
            int maxCandVisible = std::max(1, consoleH - kReserve - /*tooltip*/1);
            int visibleRows    = std::min(numCandRows, maxCandVisible);
            int acH            = visibleRows + 1; // +1 for tooltip row

            SHORT acStart       = m_acResizeFn(acH); // layout redrawn, AC area cleared
            m_acAreaStartRow    = acStart;
            m_lastCandidates    = candidates;
            m_candRegs.clear();

            for (int r = 0; r < visibleRows; ++r)
            {
                SHORT row = static_cast<SHORT>(acStart + 1 + r);
                int firstIdx = r * numCols;
                std::string rowStr;
                for (int col = 0; col < numCols; ++col)
                {
                    int idx = firstIdx + col;
                    if (idx >= (int)candidates.size()) break;
                    rowStr += candidates[idx];
                    bool hasMore = (col + 1 < numCols) && (idx + 1 < (int)candidates.size());
                    if (hasMore)
                        rowStr += std::string(colW - ColWidth(candidates[idx]), ' ');
                }
                WriteLineAt(row, rowStr);

                for (int col = 0; col < numCols; ++col)
                {
                    int idx = firstIdx + col;
                    if (idx >= (int)candidates.size()) break;
                    SHORT c0 = static_cast<SHORT>(col * colW);
                    SHORT c1 = static_cast<SHORT>(c0 + ColWidth(candidates[idx]));
                    m_candRegs.push_back({row, c0, c1, idx});
                }
            }
        }
        else if (m_splitMode && m_splitLogFn)
        {
            // Legacy split mode: send formatted rows to the cmd pane
            int col = 0;
            std::string row;
            for (const auto& c : candidates)
            {
                row += c;
                ++col;
                if (col == numCols)
                {
                    m_splitLogFn(row);
                    row.clear();
                    col = 0;
                }
                else
                    row += std::string(colW - ColWidth(c), ' ');
            }
            if (!row.empty()) m_splitLogFn(row);

            // Approximate candidate positions for hover tooltip
            SHORT inputRow2 = m_splitGetRowFn ? m_splitGetRowFn() : m_splitInputRow;
            m_candBaseRow    = static_cast<SHORT>(inputRow2 - numCandRows);
            m_lastCandidates = candidates;
            m_candRegs.clear();
            for (int i = 0; i < (int)candidates.size(); ++i)
            {
                SHORT r  = static_cast<SHORT>(m_candBaseRow + i / numCols);
                SHORT c0 = static_cast<SHORT>((i % numCols) * colW);
                SHORT c1 = static_cast<SHORT>(c0 + ColWidth(candidates[i]));
                m_candRegs.push_back({r, c0, c1, i});
            }
        }
        else
        {
            // Normal (non-split) mode: print candidates below the prompt line
            DWORD written;
            SHORT candStartRow = m_row;
            FillConsoleOutputCharacterW(m_hOut, L' ', (DWORD)candBufW, {0, m_row}, &written);
            FillConsoleOutputAttribute(m_hOut, csbiC.wAttributes, (DWORD)candBufW, {0, m_row}, &written);
            SetConsoleCursorPosition(m_hOut, {0, m_row});

            int col = 0;
            for (const auto& c : candidates)
            {
                WriteNarrow(c);
                ++col;
                if (col == numCols)
                {
                    WriteConsoleW(m_hOut, L"\r\n", 2, nullptr, nullptr);
                    col = 0;
                }
                else
                    WriteNarrow(std::string(colW - ColWidth(c), ' '));
            }
            if (col != 0)
                WriteConsoleW(m_hOut, L"\r\n", 2, nullptr, nullptr);

            GetConsoleScreenBufferInfo(m_hOut, &csbiC);
            m_row = csbiC.dwCursorPosition.Y;

            m_candBaseRow    = candStartRow;
            m_lastCandidates = candidates;
            m_candRegs.clear();
            for (int i = 0; i < (int)candidates.size(); ++i)
            {
                SHORT row = static_cast<SHORT>(candStartRow + i / numCols);
                SHORT c0  = static_cast<SHORT>((i % numCols) * colW);
                SHORT c1  = static_cast<SHORT>(c0 + ColWidth(candidates[i]));
                m_candRegs.push_back({row, c0, c1, i});
            }
        }
        Redraw(prompt);
    }

    // ── Tooltip helpers ────────────────────────────────────────────────────

    void ShowTooltip(const std::string& desc)
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);

        // When AC pane is active, row 0 of the AC area is the dedicated tooltip row.
        // Otherwise place tooltip one row above the candidate list (or below prompt).
        SHORT tooltipRow;
        if (m_acAreaStartRow >= 0)
            tooltipRow = m_acAreaStartRow;
        else if (m_candBaseRow > 0)
            tooltipRow = m_candBaseRow - 1;
        else if (m_row + 1 < csbi.dwSize.Y)
            tooltipRow = m_row + 1;
        else
            return;

        m_tooltipRow    = tooltipRow;
        m_tooltipActive = true;

        COORD pos{ 0, tooltipRow };
        DWORD written;
        FillConsoleOutputCharacterW(m_hOut, L' ', csbi.dwSize.X, pos, &written);
        SetConsoleCursorPosition(m_hOut, pos);
        if (!desc.empty())
        {
            SetConsoleTextAttribute(m_hOut, FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
            WriteNarrow(desc);
            SetConsoleTextAttribute(m_hOut, csbi.wAttributes);
        }

        // Restore cursor to input position
        int col = ColWidth(m_prompt) + ColWidth(m_buf.substr(0, m_cursor));
        if (col >= csbi.dwSize.X) col = csbi.dwSize.X - 1;
        SetConsoleCursorPosition(m_hOut, { (SHORT)col, m_row });
    }

    void ClearTooltip()
    {
        if (!m_tooltipActive) return;
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        COORD pos{ 0, m_tooltipRow };
        DWORD written;
        FillConsoleOutputCharacterW(m_hOut, L' ', csbi.dwSize.X, pos, &written);
        m_tooltipActive = false;

        int col = ColWidth(m_prompt) + ColWidth(m_buf.substr(0, m_cursor));
        if (col >= csbi.dwSize.X) col = csbi.dwSize.X - 1;
        SetConsoleCursorPosition(m_hOut, { (SHORT)col, m_row });
    }

    void ClearCandidateState()
    {
        ClearTooltip();
        if (!m_candRegs.empty() || m_acAreaStartRow >= 0)
        {
            if (m_acClearFn) m_acClearFn(); // collapse AC pane in SplitConsole
            m_acAreaStartRow = -1;
            m_candRegs.clear();
            m_lastCandidates.clear();
            m_hoveredCand = -1;
            m_candBaseRow = -1;
        }
    }

    // ── Members ───────────────────────────────────────────────────────────

    HANDLE m_hIn, m_hOut;

    std::string m_buf;
    int         m_cursor  = 0;
    SHORT       m_row     = 0;
    std::string m_prompt;  // last prompt string, used by tooltip helpers

    int                     m_histMaxLen;
    int                     m_histIdx;
    std::deque<std::string> m_history;
    std::string             m_histScratch;

    WinCompletionCallback  m_completionCb;
    WinHintCallback        m_hintCb;
    WinDescriptionCallback m_descCb;

    // Split-console state
    bool                                    m_splitMode     = false;
    SHORT                                   m_splitInputRow = 0;
    std::mutex*                             m_splitMutex    = nullptr;
    std::function<void(const std::string&)> m_splitLogFn;
    std::function<SHORT()>                  m_splitResizeFn;
    std::function<SHORT()>                  m_splitGetRowFn;
    std::function<void(int)>                m_scrollFn;

    // Viewport size tracking (drag-resize detection)
    SHORT m_viewW = 0;
    SHORT m_viewH = 0;

    // Dedicated AC pane callbacks (set via SetAcMode)
    std::function<SHORT(int)> m_acResizeFn; // set AC pane height; returns AC start row
    std::function<void()>     m_acClearFn;  // collapse AC pane
    SHORT                     m_acAreaStartRow = -1; // tooltip row in AC pane; -1 when unused

    // ── Filter mode state ─────────────────────────────────────────────────
    bool        m_filterMode    = false;   // true while editing the filter string
    std::string m_filterBuf;               // persists across mode toggles
    std::string m_savedBuf;                // command buf snapshot taken on Ctrl+F enter
    int         m_savedCursor   = 0;
    std::string m_savedPrompt;
    std::function<void(const std::string&)> m_filterChangeFn;
    std::function<void()>                   m_filterClearFn;

    // ── Mouse selection state ─────────────────────────────────────────────
    bool  m_lmbHeld     = false;   // LMB currently down (tracked across events)
    COORD m_lastMousePos{};        // last known mouse position (for scroll-while-held)
    std::function<void(COORD, bool)> m_mouseDownFn;
    std::function<void(COORD)>       m_mouseDragFn;
    std::function<void(COORD)>       m_mouseUpFn;
    std::function<std::string()>     m_copyFn;
    std::function<void()>            m_selClearFn; // RMB — clear selection
    std::function<bool(COORD)>       m_revealFilteredLineFn; // double-click while filtering
    std::function<bool(COORD)>       m_peekFn;               // double-click → open full-text peek
    std::function<bool()>            m_closePeekFn;          // dismiss peek overlay
    std::function<bool()>            m_peekActiveFn;         // is a peek currently open?

    // Candidate hover tooltip state
    std::vector<CandRegion>  m_candRegs;
    std::vector<std::string> m_lastCandidates;
    SHORT                    m_candBaseRow   = -1;
    int                      m_hoveredCand   = -1;
    bool                     m_tooltipActive = false;
    SHORT                    m_tooltipRow    = -1;
};
