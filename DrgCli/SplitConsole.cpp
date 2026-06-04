// SplitConsole.cpp — Windows Console API 3-pane split layout implementation.

#include "SplitConsole.h"
#include <cctype>   // ::tolower
#include <climits>  // INT_MAX

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
    int total  = m_filterActive ? (int)m_filteredIndices.size() : (int)m_logHistory.size();
    int maxOff = std::max(0, total - logH);
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
                                    WORD attr, const std::string& tag)
{
    std::wstring line;
    if (!tag.empty())
    {
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

// Builds the centred label for the log divider:
//   filter active → "[~query~]"
//   scrolled      → "↑ +N"
//   both          → "[~query~] ↑ +N"
//   neither       → "" (plain dashes)
std::string SplitConsole::BuildLogDivTag() const
{
    if (m_peekActive)
        return " [PEEK \xe2\x80\x94 press any key to close] ";

    std::string tag;
    if (m_filterActive)
    {
        std::string fs = m_filterStr;
        constexpr size_t kMaxDisplay = 20;
        if (fs.size() > kMaxDisplay) { fs.resize(kMaxDisplay - 2); fs += ".."; }
        tag = "[~" + fs + "~]";
    }
    if (m_scrollOffset > 0)
    {
        if (!tag.empty()) tag += ' ';
        tag += "\xe2\x86\x91 +" + std::to_string(m_scrollOffset);
    }
    return tag.empty() ? tag : " " + tag + " ";
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

    // Log pane (filter-aware)
    {
        int logH   = m_logDivRow;
        int total  = m_filterActive ? (int)m_filteredIndices.size() : (int)m_logHistory.size();
        int endIdx = total - m_scrollOffset;
        if (endIdx < 0) endIdx = 0;
        int startIdx = endIdx - logH;
        if (startIdx < 0) startIdx = 0;
        int count    = endIdx - startIdx;
        int baseRow  = logH - count;
        for (int i = 0; i < count; ++i)
        {
            int hi = m_filterActive ? (int)m_filteredIndices[startIdx + i] : (startIdx + i);
            FillLineInto(buf, w, baseRow + i, m_logHistory[hi], attr);
        }
        ApplySelectionToBuffer(buf, w);
    }

    FillDividerInto(buf, w, m_logDivRow, divAttr, BuildLogDivTag());

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

    FillDividerInto(buf, w, m_cliDivRow, divAttr, {});

    COORD      sz{w, rows};
    COORD      origin{0, 0};
    SMALL_RECT dest{0, 0, static_cast<SHORT>(w - 1), static_cast<SHORT>(rows - 1)};
    WriteConsoleOutputW(m_hOut, buf.data(), sz, origin, &dest);
    SetConsoleCursorPosition(m_hOut, {0, m_inputRow});
}

void SplitConsole::RenderLogPane()
{
    if (m_peekActive) { RenderPeekOverlay(); return; }

    int logH = m_logDivRow;
    if (logH <= 0) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(m_hOut, &csbi);
    SHORT w   = csbi.dwSize.X;
    WORD attr = csbi.wAttributes;

    int total  = m_filterActive ? (int)m_filteredIndices.size() : (int)m_logHistory.size();
    int endIdx = total - m_scrollOffset;
    if (endIdx < 0) endIdx = 0;
    int startIdx = endIdx - logH;
    if (startIdx < 0) startIdx = 0;
    int count    = endIdx - startIdx;
    int baseRow  = logH - count;

    std::vector<CHAR_INFO> buf(logH * w);
    for (auto& ci : buf) { ci.Char.UnicodeChar = L' '; ci.Attributes = attr; }
    for (int i = 0; i < count; ++i)
    {
        int hi = m_filterActive ? (int)m_filteredIndices[startIdx + i] : (startIdx + i);
        FillLineInto(buf, w, baseRow + i, m_logHistory[hi], attr);
    }
    ApplySelectionToBuffer(buf, w);

    COORD      sz{w, static_cast<SHORT>(logH)};
    COORD      origin{0, 0};
    SMALL_RECT dest{0, 0, static_cast<SHORT>(w - 1), static_cast<SHORT>(logH - 1)};
    WriteConsoleOutputW(m_hOut, buf.data(), sz, origin, &dest);
}

// Greedy word-wrap a wide string to `w` columns. Breaks on the last space that
// fits; hard-breaks any single token longer than a line.
static std::vector<std::wstring> WrapWide(const std::wstring& s, int w)
{
    std::vector<std::wstring> out;
    if (w <= 0) return out;
    size_t pos = 0;
    while (pos < s.size())
    {
        if ((int)(s.size() - pos) <= w) { out.push_back(s.substr(pos)); break; }
        size_t brk = s.rfind(L' ', pos + w - 1);
        if (brk == std::wstring::npos || brk <= pos)
        {
            out.push_back(s.substr(pos, w));   // no break point — hard split
            pos += w;
        }
        else
        {
            out.push_back(s.substr(pos, brk - pos));
            pos = brk + 1;                     // skip the break space
        }
    }
    return out;
}

// Full-text overlay for a single peeked log line, word-wrapped across the whole
// log pane. Replaces the normal log view until the peek is dismissed.
void SplitConsole::RenderPeekOverlay()
{
    int logH = m_logDivRow;
    if (logH <= 0) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(m_hOut, &csbi);
    SHORT w   = csbi.dwSize.X;
    WORD attr = csbi.wAttributes;

    std::wstring wl;
    if (!m_peekText.empty())
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, m_peekText.c_str(), (int)m_peekText.size(), nullptr, 0);
        if (n > 0)
        {
            wl.resize(n);
            MultiByteToWideChar(CP_UTF8, 0, m_peekText.c_str(), (int)m_peekText.size(), wl.data(), n);
        }
    }

    std::vector<std::wstring> rows = WrapWide(wl, w);

    std::vector<CHAR_INFO> buf(logH * w);
    for (auto& ci : buf) { ci.Char.UnicodeChar = L' '; ci.Attributes = attr; }

    auto putRow = [&](int row, const std::wstring& s)
    {
        int base = row * w;
        for (int c = 0; c < w; ++c)
            buf[base + c].Char.UnicodeChar = (c < (int)s.size()) ? s[c] : L' ';
    };

    const bool truncated = (int)rows.size() > logH;
    const int  shown     = truncated ? logH - 1 : (int)rows.size();
    for (int i = 0; i < shown; ++i) putRow(i, rows[i]);
    if (truncated)
        putRow(logH - 1,
            L"… (" + std::to_wstring((int)rows.size() - shown) +
            L" more lines — widen the window to see all)");

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
    FillDividerInto(buf, w, 0, divAttr, BuildLogDivTag());

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
    FillDividerInto(buf, w, 0, divAttr, {});
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
    bool popped = false;
    if (m_logHistory.size() > kLogMax) { m_logHistory.pop_front(); popped = true; }

    if (m_filterActive)
    {
        if (popped)
        {
            // History shifted left by one: decrement all stored indices and
            // drop any that referenced the now-removed element (was at index 0).
            size_t wi = 0;
            for (size_t idx : m_filteredIndices)
                if (idx > 0) m_filteredIndices[wi++] = idx - 1;
            m_filteredIndices.resize(wi);
        }

        // Check whether the newly appended line matches the filter.
        std::string lo = m_logHistory.back();
        std::string fs = m_filterStr;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        std::transform(fs.begin(), fs.end(), fs.begin(), ::tolower);
        bool matched = lo.find(fs) != std::string::npos;
        if (matched)
            m_filteredIndices.push_back(m_logHistory.size() - 1);

        // Scroll-pin: keep the viewport offset if the user is scrolled up.
        if (m_scrollOffset > 0 && matched)
        {
            ++m_scrollOffset;
            int maxOff = std::max(0, (int)m_filteredIndices.size() - m_logDivRow);
            if (m_scrollOffset > maxOff) m_scrollOffset = maxOff;
            DrawLogDivider();
            return;
        }

        // No visible change (no match, no pop that dirtied the view).
        if (!matched && !popped) return;

        COORD cur = GetCursorPos();
        RenderLogPane();
        DrawLogDivider();
        SetConsoleCursorPosition(m_hOut, cur);
        return;
    }

    // ── Normal (unfiltered) path ──────────────────────────────────────────
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
    int total  = m_filterActive ? (int)m_filteredIndices.size() : (int)m_logHistory.size();
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

// ─────────────────────────────────────────────────────────────────────────────
//  Filter API
// ─────────────────────────────────────────────────────────────────────────────

// Rebuild the filtered index from the full history, then re-render.
// Called under the split mutex (typically from WinInput's filterChangeFn).
// Uses RenderLogPane + DrawLogDivider rather than RenderAll so the cursor
// position written by WinInput::Redraw is preserved.
void SplitConsole::SetFilterUnderLock(const std::string& str)
{
    m_filterStr    = str;
    m_filterActive = !str.empty();
    m_filteredIndices.clear();
    m_scrollOffset = 0;

    if (m_filterActive)
    {
        std::string lf = str;
        std::transform(lf.begin(), lf.end(), lf.begin(), ::tolower);
        for (size_t i = 0; i < m_logHistory.size(); ++i)
        {
            std::string lo = m_logHistory[i];
            std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
            if (lo.find(lf) != std::string::npos)
                m_filteredIndices.push_back(i);
        }
    }

    COORD cur = GetCursorPos();
    RenderLogPane();
    DrawLogDivider();
    SetConsoleCursorPosition(m_hOut, cur);
}

void SplitConsole::ClearFilterUnderLock()
{
    m_filterActive = false;
    m_filterStr.clear();
    m_filteredIndices.clear();
    m_scrollOffset = 0;

    COORD cur = GetCursorPos();
    RenderLogPane();
    DrawLogDivider();
    SetConsoleCursorPosition(m_hOut, cur);
}

bool SplitConsole::RevealFilteredLineUnderLock(COORD pos)
{
    if (!m_filterActive) return false;

    int logH = m_logDivRow;
    if (logH <= 0) return false;
    if (pos.Y < 0 || pos.Y >= logH) return false;   // only react inside the log pane

    // Resolve the clicked line to an absolute m_logHistory index using the CURRENT
    // (filtered) view — this must happen before we drop the filter.
    SelCoord coord = ScreenToHistory(pos);
    if (!coord.valid()) return false;
    const int hi       = coord.historyIdx;
    const int clickRow = std::max(0, std::min((int)pos.Y, logH - 1));

    // Drop the filter and any in-progress/finished selection.
    m_filterActive = false;
    m_filterStr.clear();
    m_filteredIndices.clear();
    m_selActive  = false;
    m_selDone    = false;
    m_selAnchor  = {};
    m_selCurrent = {};

    // Keep the clicked line on the same screen row so it doesn't visually jump.
    // Full pane: row = hi - (endIdx - logH) and endIdx = total - scrollOffset, so
    //   scrollOffset = total - hi - logH + row.
    // Clamping to [0, maxOff] handles the "line near the top of the buffer" case
    // by landing on the oldest page (closest position that still exists).
    const int total  = (int)m_logHistory.size();
    const int maxOff = std::max(0, total - logH);
    int scrollOff = total - hi - logH + clickRow;
    if (scrollOff < 0)      scrollOff = 0;
    if (scrollOff > maxOff) scrollOff = maxOff;
    m_scrollOffset = scrollOff;

    COORD cur = GetCursorPos();
    RenderLogPane();
    DrawLogDivider();
    SetConsoleCursorPosition(m_hOut, cur);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Peek API
// ─────────────────────────────────────────────────────────────────────────────

bool SplitConsole::PeekLineUnderLock(COORD pos)
{
    if (m_peekActive)   return false;   // already peeking
    if (m_filterActive) return false;   // double-click reveals filtered lines instead

    int logH = m_logDivRow;
    if (logH <= 0) return false;
    if (pos.Y < 0 || pos.Y >= logH) return false;   // only react inside the log pane

    SelCoord coord = ScreenToHistory(pos);
    if (!coord.valid()) return false;
    const int hi = coord.historyIdx;
    if (hi < 0 || hi >= (int)m_logHistory.size()) return false;

    m_peekText   = m_logHistory[hi];
    m_peekActive = true;

    // Drop the nascent 1-line selection from the leading click of the double.
    m_selActive = false; m_selDone = false; m_selAnchor = {}; m_selCurrent = {};

    COORD cur = GetCursorPos();
    RenderLogPane();        // delegates to RenderPeekOverlay while m_peekActive
    DrawLogDivider();
    SetConsoleCursorPosition(m_hOut, cur);
    return true;
}

bool SplitConsole::ClosePeekUnderLock()
{
    if (!m_peekActive) return false;
    m_peekActive = false;
    m_peekText.clear();

    COORD cur = GetCursorPos();
    RenderLogPane();
    DrawLogDivider();
    SetConsoleCursorPosition(m_hOut, cur);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Selection API
// ─────────────────────────────────────────────────────────────────────────────

// Map a screen COORD → history-relative SelCoord.
// Row is clamped to the log pane; invalid if the log pane is empty.
SplitConsole::SelCoord SplitConsole::ScreenToHistory(COORD pos) const
{
    SelCoord result;
    int logH = m_logDivRow;
    if (logH <= 0) return result;

    int total  = m_filterActive ? (int)m_filteredIndices.size() : (int)m_logHistory.size();
    int endIdx = total - m_scrollOffset;
    if (endIdx < 0) endIdx = 0;
    int startIdx = endIdx - logH;
    if (startIdx < 0) startIdx = 0;
    int count    = endIdx - startIdx;
    int baseRow  = logH - count;

    if (count == 0) return result;

    // Clamp row to the range that has actual content.
    int row = std::max(baseRow, std::min((int)pos.Y, logH - 1));
    int logicalIdx = startIdx + (row - baseRow);
    if (logicalIdx < 0 || logicalIdx >= startIdx + count) return result;

    result.historyIdx = m_filterActive
        ? (int)m_filteredIndices[logicalIdx]
        : logicalIdx;
    result.col = std::max(0, (int)pos.X);
    return result;
}

// Apply selection highlight over a CHAR_INFO buffer that covers rows
// 0 .. m_logDivRow-1 (the log pane portion, same layout as RenderLogPane).
void SplitConsole::ApplySelectionToBuffer(std::vector<CHAR_INFO>& buf, SHORT w) const
{
    if (!m_selActive && !m_selDone) return;
    if (!m_selAnchor.valid() || !m_selCurrent.valid()) return;

    // Bright white text on dark blue, matching the AC hover colour.
    constexpr WORD kSelAttr = BACKGROUND_BLUE
                            | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE
                            | FOREGROUND_INTENSITY;

    int logH   = m_logDivRow;
    int total  = m_filterActive ? (int)m_filteredIndices.size() : (int)m_logHistory.size();
    int endIdx = total - m_scrollOffset;
    if (endIdx < 0) endIdx = 0;
    int startIdx = endIdx - logH;
    if (startIdx < 0) startIdx = 0;
    int count    = endIdx - startIdx;
    int baseRow  = logH - count;

    int minHist = std::min(m_selAnchor.historyIdx, m_selCurrent.historyIdx);
    int maxHist = std::max(m_selAnchor.historyIdx, m_selCurrent.historyIdx);

    // Column range for rectangular mode.
    int minCol = m_selRect ? std::min(m_selAnchor.col, m_selCurrent.col) : 0;
    int maxCol = m_selRect ? std::max(m_selAnchor.col, m_selCurrent.col) : (int)w - 1;

    for (int i = 0; i < count; ++i)
    {
        int hi = m_filterActive ? (int)m_filteredIndices[startIdx + i] : (startIdx + i);
        if (hi < minHist || hi > maxHist) continue;

        int bufRow = baseRow + i;
        int c0 = minCol;
        int c1 = std::min(maxCol, (int)w - 1);
        for (int c = c0; c <= c1; ++c)
            buf[bufRow * w + c].Attributes = kSelAttr;
    }
}

void SplitConsole::OnMouseDownUnderLock(COORD pos, bool rectMode)
{
    SelCoord coord = ScreenToHistory(pos);
    if (!coord.valid()) { ClearSelectionUnderLock(); return; }

    m_selAnchor  = coord;
    m_selCurrent = coord;
    m_selRect    = rectMode;
    m_selActive  = true;
    m_selDone    = false;

    COORD cur = GetCursorPos();
    RenderLogPane();
    SetConsoleCursorPosition(m_hOut, cur);
}

void SplitConsole::OnMouseDragUnderLock(COORD pos)
{
    if (!m_selActive) return;
    SelCoord coord = ScreenToHistory(pos);
    if (!coord.valid()) return;

    m_selCurrent = coord;

    COORD cur = GetCursorPos();
    RenderLogPane();
    SetConsoleCursorPosition(m_hOut, cur);
}

void SplitConsole::OnMouseUpUnderLock(COORD pos)
{
    if (!m_selActive) return;
    SelCoord coord = ScreenToHistory(pos);
    if (coord.valid()) m_selCurrent = coord;

    m_selActive = false;
    m_selDone   = true;

    COORD cur = GetCursorPos();
    RenderLogPane();
    SetConsoleCursorPosition(m_hOut, cur);
}

void SplitConsole::ClearSelectionUnderLock()
{
    if (!m_selActive && !m_selDone) return;
    m_selActive  = false;
    m_selDone    = false;
    m_selAnchor  = {};
    m_selCurrent = {};

    COORD cur = GetCursorPos();
    RenderLogPane();
    SetConsoleCursorPosition(m_hOut, cur);
}

// Build clipboard text for the current selection, applying the current filter
// so the filter at copy-time determines what's included, not at select-time.
std::string SplitConsole::CopySelectionUnderLock() const
{
    if (!m_selActive && !m_selDone) return {};
    if (!m_selAnchor.valid() || !m_selCurrent.valid()) return {};

    int minHist = std::min(m_selAnchor.historyIdx, m_selCurrent.historyIdx);
    int maxHist = std::max(m_selAnchor.historyIdx, m_selCurrent.historyIdx);

    // Current-filter lower-case string for matching at copy time.
    std::string lf;
    if (m_filterActive)
    {
        lf = m_filterStr;
        std::transform(lf.begin(), lf.end(), lf.begin(), ::tolower);
    }

    int minCol = m_selRect ? std::min(m_selAnchor.col, m_selCurrent.col) : 0;
    int maxCol = m_selRect ? std::max(m_selAnchor.col, m_selCurrent.col) : INT_MAX;

    std::string result;
    for (int hi = minHist; hi <= maxHist; ++hi)
    {
        if (hi < 0 || hi >= (int)m_logHistory.size()) continue;
        const std::string& line = m_logHistory[hi];

        // Apply current filter.
        if (m_filterActive)
        {
            std::string lo = line;
            std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
            if (lo.find(lf) == std::string::npos) continue;
        }

        if (m_selRect)
        {
            // Extract only the column range (treat as byte columns; fine for ASCII log text).
            int end = std::min(maxCol + 1, (int)line.size());
            if (minCol < (int)line.size())
                result += line.substr(minCol, end - minCol);
            result += '\n';
        }
        else
        {
            result += line + '\n';
        }
    }
    return result;
}
