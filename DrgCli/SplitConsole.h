#pragma once
// SplitConsole.h — Windows Console API 3-pane split layout.
//
//  Layout (all rows are absolute screen-buffer rows, viewport pinned at 0):
//    0 .. logDivRow-1              — LOG pane  (large; DLL output; PgUp/PgDn to scroll)
//    logDivRow                     — top divider ─────────── [↑ +N] ───────────
//    logDivRow+1 .. cliDivRow-1   — CLI output area (kCliH rows, fixed)
//    cliDivRow                     — bottom divider (only when AC pane is active)
//    cliDivRow+1 .. inputRow-1    — AC pane (autocomplete candidates + tooltip row)
//    inputRow                      — INPUT prompt  (owned by WinInput)
//
//  cliDivRow is always present (bottom divider always visible):
//    cliDivRow = consH - m_acH - 2   (with m_acH == 0: cliDivRow = consH - 2)
//    logDivRow = cliDivRow - kCliH - 1
//
//  Thread safety:
//    PrintLog / PrintCmd / ScrollLog  — acquire mutex themselves (external threads)
//    *UnderLock variants              — caller already holds Mutex() (WinInput callbacks)

#include <Windows.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <string>

class SplitConsole
{
    static constexpr unsigned long long kCliH   = 3;    // fixed CLI output rows (excluding prompt row)
    static constexpr unsigned long long kLogMax = 1<<31; // log history ring size
    static constexpr unsigned long long kCmdMax = 200;  // CLI history ring size

public:
    SplitConsole()
        : m_hOut(GetStdHandle(STD_OUTPUT_HANDLE))
        , m_acH(0)
        , m_consH(0), m_logDivRow(0), m_cliDivRow(0), m_inputRow(0)
        , m_scrollOffset(0)
    {
        RecalcLayout();
        ClearAll();
        DrawLogDivider();
    }

    // ── Public API ────────────────────────────────────────────────────────

    // DLL log output (external thread)
    void PrintLog(const std::string& line)
    {
        std::lock_guard lk(m_mutex);
        PrintLogLocked(line);
    }
    // WinInput callback (mutex already held)
    void PrintLogUnderLock(const std::string& line) { PrintLogLocked(line); }

    // CLI output: responses, system messages (external thread)
    void PrintCmd(const std::string& line)
    {
        std::lock_guard lk(m_mutex);
        PrintCmdLocked(line);
    }
    // WinInput callback (mutex already held)
    void PrintCmdUnderLock(const std::string& line) { PrintCmdLocked(line); }

    // Scroll the log pane. delta > 0 = older content, delta < 0 = newer.
    void ScrollLog(int delta)
    {
        std::lock_guard lk(m_mutex);
        ScrollLogLocked(delta);
    }
    void ScrollLogUnderLock(int delta) { ScrollLogLocked(delta); }

    SHORT      InputRow()  const { return m_inputRow; }
    SHORT      LogDivRow() const { return m_logDivRow; }
    std::mutex& Mutex()          { return m_mutex; }

    // Resize — caller must hold Mutex()
    void HandleResizeLocked()
    {
        m_acH = 0; // AC is stale after resize; WinInput will re-show if needed
        RecalcLayout();
        RenderAll();
    }

    // Called by WinInput when showing autocomplete candidates.
    // Sets the AC pane to acH rows, redraws the layout, clears the AC area,
    // and returns the first row of the AC area (the tooltip row).
    // Candidate rows begin at returnValue + 1.
    // Caller must hold Mutex().
    SHORT SetAcHeightUnderLock(int acH)
    {
        if (acH == m_acH && m_acH > 0)
            return static_cast<SHORT>(m_cliDivRow + 1);

        m_acH = acH;
        RecalcLayout();
        RenderAll();
        return static_cast<SHORT>(m_cliDivRow + 1);
    }

    // Called by WinInput when AC is dismissed (single match or LCP extension).
    // Caller must hold Mutex().
    void ClearAcUnderLock()
    {
        if (m_acH == 0) return;
        m_acH = 0;
        RecalcLayout();
        RenderAll();
    }

private:
    // ── Layout ────────────────────────────────────────────────────────────

    void RecalcLayout()
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        m_consH = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

        // Minimum: 1 log row + top div + kCliH cli rows + bottom div + input
        const int kMin = kCliH + 4;
        if (m_consH < kMin) m_consH = kMin;

        m_inputRow  = static_cast<SHORT>(m_consH - 1);
        // Bottom divider is always present; AC rows (if any) sit between it and input.
        m_cliDivRow = static_cast<SHORT>(m_consH - m_acH - 2);
        m_logDivRow = static_cast<SHORT>(m_cliDivRow - kCliH - 1);

        // Ensure log pane always has at least 1 row
        if (m_logDivRow < 1)
        {
            m_logDivRow = 1;
            m_cliDivRow = static_cast<SHORT>(m_logDivRow + kCliH + 1);
        }

        // Clamp scroll offset to new log pane height
        int logH   = m_logDivRow;
        int maxOff = std::max(0, (int)m_logHistory.size() - logH);
        if (m_scrollOffset > maxOff) m_scrollOffset = maxOff;

        // Scroll viewport back to row 0 only if it has drifted — calling
        // SetConsoleWindowInfo unconditionally fires another WINDOW_BUFFER_SIZE_EVENT
        // which causes a resize loop in windowed mode.
        SHORT w = ConsoleWidth();
        if (csbi.srWindow.Top != 0)
        {
            SMALL_RECT win{0, 0, static_cast<SHORT>(w - 1), static_cast<SHORT>(m_consH - 1)};
            SetConsoleWindowInfo(m_hOut, TRUE, &win);
        }
    }

    SHORT ConsoleWidth() const
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        return csbi.dwSize.X;
    }

    // ── Rendering ─────────────────────────────────────────────────────────

    void RenderAll()
    {
        ClearAll();
        RenderLogPane();
        DrawLogDivider();
        RenderCliPane();
        DrawCliDivider();
        SetConsoleCursorPosition(m_hOut, {0, m_inputRow});
    }

    void ClearAll()
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        SHORT w = csbi.dwSize.X;
        DWORD written;
        for (SHORT r = 0; r < m_consH; ++r)
        {
            COORD pos{0, r};
            FillConsoleOutputCharacterW(m_hOut, L' ', w, pos, &written);
            FillConsoleOutputAttribute(m_hOut, csbi.wAttributes, w, pos, &written);
        }
    }

    void DrawLogDivider()
    {
        SHORT w = ConsoleWidth();
        COORD pos{0, m_logDivRow};
        std::wstring line;

        if (m_scrollOffset > 0)
        {
            std::string tag = " \xe2\x86\x91 +" + std::to_string(m_scrollOffset) + " ";
            int wlen = MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), (int)tag.size(), nullptr, 0);
            std::wstring wtag(wlen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), (int)tag.size(), wtag.data(), wlen);
            int half = (w - (int)wtag.size()) / 2;
            if (half < 0) half = 0;
            line = std::wstring(half, L'─') + wtag
                 + std::wstring(w - half - (int)wtag.size(), L'─');
        }
        else
        {
            line = std::wstring(w, L'─');
        }
        if ((int)line.size() > w) line.resize(w);

        WORD attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        DWORD written;
        FillConsoleOutputAttribute(m_hOut, attr, w, pos, &written);
        WriteConsoleOutputCharacterW(m_hOut, line.c_str(), (DWORD)line.size(), pos, &written);
    }

    void DrawCliDivider()
    {
        SHORT w = ConsoleWidth();
        COORD pos{0, m_cliDivRow};
        std::wstring line(w, L'─');
        WORD attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        DWORD written;
        FillConsoleOutputAttribute(m_hOut, attr, w, pos, &written);
        WriteConsoleOutputCharacterW(m_hOut, line.c_str(), (DWORD)line.size(), pos, &written);
    }

    void WriteLineAt(SHORT row, const std::string& utf8)
    {
        SHORT w = ConsoleWidth();
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
        if (!wline.empty())
            WriteConsoleOutputCharacterW(m_hOut, wline.c_str(), (DWORD)wline.size(), pos, &written);
    }

    void RenderLogPane()
    {
        int logH = m_logDivRow;
        if (logH <= 0) return;

        int total  = (int)m_logHistory.size();
        int endIdx = total - m_scrollOffset;
        if (endIdx < 0) endIdx = 0;
        int startIdx = endIdx - logH;
        if (startIdx < 0) startIdx = 0;

        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        SHORT w = csbi.dwSize.X;
        for (SHORT r = 0; r < logH; ++r)
        {
            COORD pos{0, r};
            DWORD written;
            FillConsoleOutputCharacterW(m_hOut, L' ', w, pos, &written);
            FillConsoleOutputAttribute(m_hOut, csbi.wAttributes, w, pos, &written);
        }
        int count    = endIdx - startIdx;
        SHORT baseRow = static_cast<SHORT>(logH - count);
        for (int i = 0; i < count; ++i)
            WriteLineAt(static_cast<SHORT>(baseRow + i), m_logHistory[startIdx + i]);
    }

    void RenderCliPane()
    {
        SHORT firstRow = static_cast<SHORT>(m_logDivRow + 1);
        SHORT lastRow  = static_cast<SHORT>(m_cliDivRow - 1);
        int cmdOutputH  = lastRow - firstRow + 1;
        if (cmdOutputH <= 0) return;

        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        SHORT w = csbi.dwSize.X;

        for (int i = 0; i < cmdOutputH; ++i)
        {
            COORD pos{0, static_cast<SHORT>(firstRow + i)};
            DWORD written;
            FillConsoleOutputCharacterW(m_hOut, L' ', w, pos, &written);
            FillConsoleOutputAttribute(m_hOut, csbi.wAttributes, w, pos, &written);
        }
        // Bottom-align history
        int total    = (int)m_cliHistory.size();
        int startIdx = total - cmdOutputH;
        if (startIdx < 0) startIdx = 0;
        int count    = total - startIdx;
        SHORT baseRow = static_cast<SHORT>(firstRow + (cmdOutputH - count));
        for (int i = 0; i < count; ++i)
            WriteLineAt(static_cast<SHORT>(baseRow + i), m_cliHistory[startIdx + i]);
    }

    // ── Core locked operations ────────────────────────────────────────────

    void PrintLogLocked(const std::string& line)
    {
        m_logHistory.push_back(line);
        while ((int)m_logHistory.size() > kLogMax) m_logHistory.pop_front();

        if (m_scrollOffset > 0)
        {
            ++m_scrollOffset;
            int maxOff = std::max(0, (int)m_logHistory.size() - m_logDivRow);
            if (m_scrollOffset > maxOff) m_scrollOffset = maxOff;
            DrawLogDivider();
            return;
        }

        RenderLogPane();
        DrawLogDivider();
        SetConsoleCursorPosition(m_hOut, {0, m_inputRow});
    }

    void PrintCmdLocked(const std::string& line)
    {
        m_cliHistory.push_back(line);
        while ((int)m_cliHistory.size() > kCmdMax) m_cliHistory.pop_front();
        RenderCliPane();
        SetConsoleCursorPosition(m_hOut, {0, m_inputRow});
    }

    void ScrollLogLocked(int delta)
    {
        int logH   = m_logDivRow;
        int total  = (int)m_logHistory.size();
        int maxOff = std::max(0, total - logH);
        int newOff = std::clamp(m_scrollOffset + delta, 0, maxOff);
        if (newOff == m_scrollOffset) return;  // already at boundary, nothing to render
        m_scrollOffset = newOff;
        RenderLogPane();
        DrawLogDivider();
        SetConsoleCursorPosition(m_hOut, {0, m_inputRow});
    }

    // ── Members ───────────────────────────────────────────────────────────

    HANDLE     m_hOut;
    std::mutex m_mutex;

    int   m_acH;        // 0 = no AC pane; > 0 = height of AC pane (rows)
    SHORT m_consH;      // console window height in rows
    SHORT m_logDivRow;  // row of the top (log/cli) divider
    SHORT m_cliDivRow;  // row of the bottom (cli/ac) divider (always present)
    SHORT m_inputRow;   // row of the prompt (always m_consH - 1)

    int m_scrollOffset; // 0 = latest; positive = scrolled back N lines

    std::deque<std::string> m_logHistory; // all DLL/injector log lines
    std::deque<std::string> m_cliHistory; // CLI responses / system messages
};
