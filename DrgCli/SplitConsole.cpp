// SplitConsole.cpp — Windows Console API 3-pane split layout implementation.

#include "SplitConsole.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────

SplitConsole::SplitConsole()
    : m_hOut(GetStdHandle(STD_OUTPUT_HANDLE))
    , m_acH(0)
    , m_consH(0), m_logDivRow(0), m_cliDivRow(0), m_inputRow(0)
    , m_scrollOffset(0)
{
    RecalcLayout();
    RenderAll();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

void SplitConsole::PrintLog(const std::string& line)
{
    std::lock_guard lk(m_mutex);
    PrintLogLocked(line);
}

void SplitConsole::PrintLogUnderLock(const std::string& line) { PrintLogLocked(line); }

void SplitConsole::PrintCmd(const std::string& line)
{
    std::lock_guard lk(m_mutex);
    PrintCmdLocked(line);
}

void SplitConsole::PrintCmdUnderLock(const std::string& line) { PrintCmdLocked(line); }

void SplitConsole::ScrollLog(int delta)
{
    std::lock_guard lk(m_mutex);
    ScrollLogLocked(delta);
}

void SplitConsole::ScrollLogUnderLock(int delta) { ScrollLogLocked(delta); }

void SplitConsole::HandleResizeLocked()
{
    m_acH = 0;
    RecalcLayout();
    RenderAll();
}

SHORT SplitConsole::SetAcHeightUnderLock(int acH)
{
    if (acH == m_acH && m_acH > 0)
        return static_cast<SHORT>(m_cliDivRow + 1);

    m_acH = acH;
    RecalcLayout();
    RenderAll();
    return static_cast<SHORT>(m_cliDivRow + 1);
}

void SplitConsole::ClearAcUnderLock()
{
    if (m_acH == 0) return;
    m_acH = 0;
    RecalcLayout();
    RenderAll();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Layout
// ─────────────────────────────────────────────────────────────────────────────

void SplitConsole::RecalcLayout()
{
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(m_hOut, &csbi);
    m_consH = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

    const int kMin = kCliH + 4;
    if (m_consH < kMin) m_consH = kMin;

    m_inputRow  = static_cast<SHORT>(m_consH - 1);
    m_cliDivRow = static_cast<SHORT>(m_consH - m_acH - 2);
    m_logDivRow = static_cast<SHORT>(m_cliDivRow - kCliH - 1);

    if (m_logDivRow < 1)
    {
        m_logDivRow = 1;
        m_cliDivRow = static_cast<SHORT>(m_logDivRow + kCliH + 1);
    }

    int logH   = m_logDivRow;
    int maxOff = std::max(0, (int)m_logHistory.size() - logH);
    if (m_scrollOffset > maxOff) m_scrollOffset = maxOff;

    SHORT w = ConsoleWidth();
    if (csbi.srWindow.Top != 0)
    {
        SMALL_RECT win{0, 0, static_cast<SHORT>(w - 1), static_cast<SHORT>(m_consH - 1)};
        SetConsoleWindowInfo(m_hOut, TRUE, &win);
    }
}

SHORT SplitConsole::ConsoleWidth() const
{
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(m_hOut, &csbi);
    return csbi.dwSize.X;
}

COORD SplitConsole::GetCursorPos() const
{
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(m_hOut, &csbi);
    return csbi.dwCursorPosition;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CHAR_INFO buffer helpers
// ─────────────────────────────────────────────────────────────────────────────

void SplitConsole::FillLineInto(std::vector<CHAR_INFO>& buf, SHORT w, int row,
                                 const std::string& utf8, WORD attr)
{
    std::wstring wl;
    if (!utf8.empty())
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
        if (n > 0)
        {
            wl.resize(n);
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wl.data(), n);
        }
    }
    if ((int)wl.size() > w) wl.resize(w);
    int base = row * w;
    for (int c = 0; c < w; ++c)
    {
        buf[base + c].Char.UnicodeChar = (c < (int)wl.size()) ? wl[c] : L' ';
        buf[base + c].Attributes       = attr;
    }
}

void SplitConsole::FillDividerInto(std::vector<CHAR_INFO>& buf, SHORT w, int row,
                                    WORD attr, int scrollIndicator)
{
    std::wstring line;
    if (scrollIndicator > 0)
    {
        std::string tag = " \xe2\x86\x91 +" + std::to_string(scrollIndicator) + " ";
        int wn = MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), (int)tag.size(), nullptr, 0);
        std::wstring wtag(wn, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), (int)tag.size(), wtag.data(), wn);
        int half = std::max(0, (w - (int)wtag.size()) / 2);
        line = std::wstring(half, L'─') + wtag
             + std::wstring(std::max(0, (int)w - half - (int)wtag.size()), L'─');
    }
    else
    {
        line = std::wstring(w, L'─');
    }
    if ((int)line.size() > w) line.resize(w);
    int base = row * w;
    for (int c = 0; c < w; ++c)
    {
        buf[base + c].Char.UnicodeChar = (c < (int)line.size()) ? line[c] : L' ';
        buf[base + c].Attributes       = attr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Rendering
// ─────────────────────────────────────────────────────────────────────────────

void SplitConsole::RenderAll()
{
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(m_hOut, &csbi);
    SHORT w    = csbi.dwSize.X;
    SHORT rows = static_cast<SHORT>(m_inputRow + 1);
    WORD  attr    = csbi.wAttributes;
    WORD  divAttr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

    std::vector<CHAR_INFO> buf(rows * w);
    for (auto& ci : buf) { ci.Char.UnicodeChar = L' '; ci.Attributes = attr; }

    // Log pane
    {
        int logH     = m_logDivRow;
        int total    = (int)m_logHistory.size();
        int endIdx   = total - m_scrollOffset;
        if (endIdx < 0) endIdx = 0;
        int startIdx = endIdx - logH;
        if (startIdx < 0) startIdx = 0;
        int count    = endIdx - startIdx;
        int baseRow  = logH - count;
        for (int i = 0; i < count; ++i)
            FillLineInto(buf, w, baseRow + i, m_logHistory[startIdx + i], attr);
    }

    FillDividerInto(buf, w, m_logDivRow, divAttr, m_scrollOffset);

    // CLI pane
    {
        int firstRow   = m_logDivRow + 1;
        int lastRow    = m_cliDivRow - 1;
        int cmdOutputH = lastRow - firstRow + 1;
        if (cmdOutputH > 0)
        {
            int total    = (int)m_cliHistory.size();
            int startIdx = total - cmdOutputH;
            if (startIdx < 0) startIdx = 0;
            int count    = total - startIdx;
            int baseRow  = firstRow + (cmdOutputH - count);
            for (int i = 0; i < count; ++i)
                FillLineInto(buf, w, baseRow + i, m_cliHistory[startIdx + i], attr);
        }
    }

    FillDividerInto(buf, w, m_cliDivRow, divAttr, 0);

    COORD      sz{w, rows};
    COORD      origin{0, 0};
    SMALL_RECT dest{0, 0, static_cast<SHORT>(w - 1), static_cast<SHORT>(rows - 1)};
    WriteConsoleOutputW(m_hOut, buf.data(), sz, origin, &dest);
    SetConsoleCursorPosition(m_hOut, {0, m_inputRow});
}

void SplitConsole::RenderLogPane()
{
    int logH = m_logDivRow;
    if (logH <= 0) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(m_hOut, &csbi);
    SHORT w   = csbi.dwSize.X;
    WORD attr = csbi.wAttributes;

    int total    = (int)m_logHistory.size();
    int endIdx   = total - m_scrollOffset;
    if (endIdx < 0) endIdx = 0;
    int startIdx = endIdx - logH;
    if (startIdx < 0) startIdx = 0;
    int count    = endIdx - startIdx;
    int baseRow  = logH - count;

    std::vector<CHAR_INFO> buf(logH * w);
    for (auto& ci : buf) { ci.Char.UnicodeChar = L' '; ci.Attributes = attr; }
    for (int i = 0; i < count; ++i)
        FillLineInto(buf, w, baseRow + i, m_logHistory[startIdx + i], attr);

    COORD      sz{w, static_cast<SHORT>(logH)};
    COORD      origin{0, 0};
    SMALL_RECT dest{0, 0, static_cast<SHORT>(w - 1), static_cast<SHORT>(logH - 1)};
    WriteConsoleOutputW(m_hOut, buf.data(), sz, origin, &dest);
}

void SplitConsole::RenderCliPane()
{
    SHORT firstRow  = static_cast<SHORT>(m_logDivRow + 1);
    SHORT lastRow   = static_cast<SHORT>(m_cliDivRow - 1);
    int cmdOutputH  = lastRow - firstRow + 1;
    if (cmdOutputH <= 0) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(m_hOut, &csbi);
    SHORT w   = csbi.dwSize.X;
    WORD attr = csbi.wAttributes;

    int total    = (int)m_cliHistory.size();
    int startIdx = total - cmdOutputH;
    if (startIdx < 0) startIdx = 0;
    int count    = total - startIdx;
    int baseRow  = cmdOutputH - count;

    std::vector<CHAR_INFO> buf(cmdOutputH * w);
    for (auto& ci : buf) { ci.Char.UnicodeChar = L' '; ci.Attributes = attr; }
    for (int i = 0; i < count; ++i)
        FillLineInto(buf, w, baseRow + i, m_cliHistory[startIdx + i], attr);

    COORD      sz{w, static_cast<SHORT>(cmdOutputH)};
    COORD      origin{0, 0};
    SMALL_RECT dest{0, firstRow, static_cast<SHORT>(w - 1), lastRow};
    WriteConsoleOutputW(m_hOut, buf.data(), sz, origin, &dest);
}

void SplitConsole::DrawLogDivider()
{
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(m_hOut, &csbi);
    SHORT w       = csbi.dwSize.X;
    WORD  divAttr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

    std::vector<CHAR_INFO> buf(w);
    FillDividerInto(buf, w, 0, divAttr, m_scrollOffset);

    COORD      sz{w, 1};
    COORD      origin{0, 0};
    SMALL_RECT dest{0, m_logDivRow, static_cast<SHORT>(w - 1), m_logDivRow};
    WriteConsoleOutputW(m_hOut, buf.data(), sz, origin, &dest);
}

void SplitConsole::DrawCliDivider()
{
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(m_hOut, &csbi);
    SHORT w       = csbi.dwSize.X;
    WORD  divAttr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

    std::vector<CHAR_INFO> buf(w);
    FillDividerInto(buf, w, 0, divAttr, 0);

    COORD      sz{w, 1};
    COORD      origin{0, 0};
    SMALL_RECT dest{0, m_cliDivRow, static_cast<SHORT>(w - 1), m_cliDivRow};
    WriteConsoleOutputW(m_hOut, buf.data(), sz, origin, &dest);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Core locked operations
// ─────────────────────────────────────────────────────────────────────────────

void SplitConsole::PrintLogLocked(const std::string& line)
{
    m_logHistory.push_back(line);
    while (m_logHistory.size() > kLogMax) m_logHistory.pop_front();

    if (m_scrollOffset > 0)
    {
        ++m_scrollOffset;
        int maxOff = std::max(0, (int)m_logHistory.size() - m_logDivRow);
        if (m_scrollOffset > maxOff) m_scrollOffset = maxOff;
        DrawLogDivider();
        return;
    }

    COORD cur = GetCursorPos();
    RenderLogPane();
    DrawLogDivider();
    SetConsoleCursorPosition(m_hOut, cur);
}

void SplitConsole::PrintCmdLocked(const std::string& line)
{
    m_cliHistory.push_back(line);
    while ((int)m_cliHistory.size() > kCmdMax) m_cliHistory.pop_front();
    COORD cur = GetCursorPos();
    RenderCliPane();
    SetConsoleCursorPosition(m_hOut, cur);
}

void SplitConsole::ScrollLogLocked(int delta)
{
    int logH   = m_logDivRow;
    int total  = (int)m_logHistory.size();
    int maxOff = std::max(0, total - logH);

    int newOff;
    if (delta >= 0)
        newOff = (maxOff - m_scrollOffset < delta) ? maxOff : m_scrollOffset + delta;
    else
        newOff = (-delta > m_scrollOffset) ? 0 : m_scrollOffset + delta;

    if (newOff == m_scrollOffset) return;
    m_scrollOffset = newOff;

    COORD cur = GetCursorPos();
    RenderLogPane();
    DrawLogDivider();
    SetConsoleCursorPosition(m_hOut, cur);
}
