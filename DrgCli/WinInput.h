#pragma once
// WinInput.h — minimal Windows console readline with Tab-completion,
//               inline ghost-text hints, and up/down history.
//               No dependencies beyond Windows.h.

#include <Windows.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <deque>

// ─────────────────────────────────────────────────────────────────────────────
//  Callback types
// ─────────────────────────────────────────────────────────────────────────────

// Fill `out` with every completion for the current line buffer.
using WinCompletionCallback = std::function<void(const std::string& buf, std::vector<std::string>& out)>;

// Hint result:
//   inline   — completion text that follows cursor inside the existing buffer
//              (rendered dim over the already-typed chars ahead of cursor)
//   append   — extra display text appended after end of buffer (params, owner, count)
//              (never inserted on right-arrow)
//   color    — Windows console attribute for both parts
struct WinHint
{
    std::string inlineText;  // chars in buf after cursor that complete the name
    std::string appendText;  // ghost text appended after end of buf
    WORD        color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
};
using WinHintCallback = std::function<WinHint(const std::string& buf, int cursor)>;

// ─────────────────────────────────────────────────────────────────────────────
//  WinInput
// ─────────────────────────────────────────────────────────────────────────────

class WinInput
{
public:
    WinInput()
        : m_hIn(GetStdHandle(STD_INPUT_HANDLE))
        , m_hOut(GetStdHandle(STD_OUTPUT_HANDLE))
        , m_histMaxLen(200)
        , m_histIdx(-1)
        , m_row(0)
    {
    }

    void SetCompletionCallback(WinCompletionCallback cb) { m_completionCb = std::move(cb); }
    void SetHintCallback(WinHintCallback cb) { m_hintCb = std::move(cb); }
    void SetHistoryMaxLen(int n) { m_histMaxLen = n; }

    void HistoryAdd(const std::string& line)
    {
        if (!line.empty() && (m_history.empty() || m_history.front() != line))
            m_history.push_front(line);
        while ((int)m_history.size() > m_histMaxLen)
            m_history.pop_back();
    }

    // Blocking readline. Returns false on Ctrl-D / EOF.
    bool Readline(const std::string& prompt, std::string& out)
    {
        DWORD prevMode = 0;
        GetConsoleMode(m_hIn, &prevMode);
        SetConsoleMode(m_hIn, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT);

        m_buf.clear();
        m_cursor = 0;
        m_histIdx = -1;
        m_histScratch.clear();

        // Record the row we start on, then print the prompt
        {
            CONSOLE_SCREEN_BUFFER_INFO csbi{};
            GetConsoleScreenBufferInfo(m_hOut, &csbi);
            m_row = csbi.dwCursorPosition.Y;
        }
        WriteNarrow(prompt);

        bool result = true;

        while (true)
        {
            INPUT_RECORD rec{};
            DWORD nRead = 0;
            ReadConsoleInputW(m_hIn, &rec, 1, &nRead);

            if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
                continue;

            const KEY_EVENT_RECORD& ke = rec.Event.KeyEvent;
            WORD    vk = ke.wVirtualKeyCode;
            wchar_t ch = ke.uChar.UnicodeChar;

            if (vk == VK_RETURN)
            {
                // Move past the input line before returning
                CONSOLE_SCREEN_BUFFER_INFO csbi{};
                GetConsoleScreenBufferInfo(m_hOut, &csbi);
                COORD eol{ 0, (SHORT)(csbi.dwCursorPosition.Y + 1) };
                // Clamp to buffer
                if (eol.Y >= csbi.dwSize.Y) eol.Y = csbi.dwSize.Y - 1;
                SetConsoleCursorPosition(m_hOut, { 0, csbi.dwCursorPosition.Y });
                WriteConsoleW(m_hOut, L"\r\n", 2, nullptr, nullptr);
                break;
            }

            if (vk == VK_ESCAPE)
            {
                m_buf.clear(); m_cursor = 0;
                Redraw(prompt);
                continue;
            }

            if (vk == VK_BACK || ch == 0x08)
            {
                if (m_cursor > 0)
                {
                    m_buf.erase(m_cursor - 1, 1);
                    --m_cursor;
                    Redraw(prompt);
                }
                continue;
            }

            if (vk == VK_DELETE)
            {
                if (m_cursor < (int)m_buf.size())
                {
                    m_buf.erase(m_cursor, 1);
                    Redraw(prompt);
                }
                continue;
            }

            if (vk == VK_LEFT)
            {
                bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                if (ctrl)
                {
                    // Jump to start of previous word
                    int c = m_cursor;
                    while (c > 0 && m_buf[c - 1] == ' ') --c; // skip spaces
                    while (c > 0 && m_buf[c - 1] != ' ') --c; // skip word chars
                    m_cursor = c;
                }
                else if (m_cursor > 0) --m_cursor;
                Redraw(prompt);
                continue;
            }

            if (vk == VK_RIGHT)
            {
                bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                if (ctrl)
                {
                    // Jump to start of next word
                    int c = m_cursor;
                    int n = (int)m_buf.size();
                    while (c < n && m_buf[c] != ' ') ++c; // skip word chars
                    while (c < n && m_buf[c] == ' ') ++c; // skip spaces
                    m_cursor = c;
                    Redraw(prompt);
                }
                else if (m_cursor < (int)m_buf.size())
                {
                    ++m_cursor;
                    Redraw(prompt);
                }
                else
                {
                    // At end of buffer: accept inlineText completion
                    WinHint hint = Hint();
                    if (!hint.inlineText.empty())
                    {
                        // inlineText is already visually shown as the dim chars
                        // ahead — accepting it just moves cursor to end of name
                        m_cursor = std::min(m_cursor + (int)hint.inlineText.size(),
                            (int)m_buf.size());
                        Redraw(prompt);
                    }
                }
                continue;
            }

            if (vk == VK_HOME) { m_cursor = 0;                    Redraw(prompt); continue; }
            if (vk == VK_END) { m_cursor = (int)m_buf.size();    Redraw(prompt); continue; }

            if (vk == VK_UP) { NavigateHistory(prompt, +1); continue; }
            if (vk == VK_DOWN) { NavigateHistory(prompt, -1); continue; }

            // Ctrl-A / Ctrl-E
            if (ch == 0x01) { m_cursor = 0;                 Redraw(prompt); continue; }
            if (ch == 0x05) { m_cursor = (int)m_buf.size(); Redraw(prompt); continue; }

            // Ctrl-D on empty line → EOF
            if (ch == 0x04 && m_buf.empty())
            {
                WriteConsoleW(m_hOut, L"\r\n", 2, nullptr, nullptr);
                result = false;
                break;
            }

            // Ctrl-K: kill to end
            if (ch == 0x0B) { m_buf.erase(m_cursor); Redraw(prompt); continue; }

            // Ctrl-U: kill to start
            if (ch == 0x15) { m_buf.erase(0, m_cursor); m_cursor = 0; Redraw(prompt); continue; }

            // Tab
            if (vk == VK_TAB) { DoCompletion(prompt); continue; }

            // Printable character
            if (ch >= 0x20 && ch != 0x7F)
            {
                char narrow[4]{};
                int bytes = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, narrow, sizeof(narrow), nullptr, nullptr);
                if (bytes > 0)
                {
                    m_buf.insert(m_cursor, narrow, bytes);
                    m_cursor += bytes;
                    Redraw(prompt);
                }
                continue;
            }
        }

        SetConsoleMode(m_hIn, prevMode);
        out = m_buf;
        return result;
    }

private:
    // ── Low-level helpers ─────────────────────────────────────────────────

    void WriteNarrow(const std::string& s)
    {
        if (s.empty()) return;
        std::wstring w(s.size() + 4, L'\0');
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], (int)w.size());
        DWORD written;
        WriteConsoleW(m_hOut, &w[0], n, &written, nullptr);
    }

    // Number of console columns a UTF-8 string occupies (BMP only).
    int ColWidth(const std::string& s)
    {
        if (s.empty()) return 0;
        return MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    }

    // ── Hint ──────────────────────────────────────────────────────────────

    WinHint Hint()
    {
        if (!m_hintCb) return {};
        return m_hintCb(m_buf, m_cursor);
    }

    // ── Redraw ────────────────────────────────────────────────────────────
    //
    // Always redraws onto m_row.  If the buffer scrolled since we printed
    // the prompt, m_row will be stale — we detect that by comparing against
    // the current cursor row and adjust m_row downward accordingly.

    void Redraw(const std::string& prompt)
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        SHORT bufW = csbi.dwSize.X;
        SHORT curRow = csbi.dwCursorPosition.Y;

        if (curRow < m_row) m_row = curRow;
        if (m_row < 0) m_row = 0;
        if (m_row >= csbi.dwSize.Y) m_row = csbi.dwSize.Y - 1;

        // Erase the row
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

        // Segment 3: inlineText (name completion ahead of cursor) in hint color
        // This visually "dims" the characters in buf that complete the current token.
        // We render the hint inline text here instead of the real buf chars,
        // because the real buf chars ahead of cursor haven't been typed yet —
        // inlineText IS those chars (the remainder of the matched name).
        if (!hint.inlineText.empty())
        {
            SetConsoleTextAttribute(m_hOut, hint.color);
            WriteNarrow(hint.inlineText);
            SetConsoleTextAttribute(m_hOut, csbi.wAttributes);
        }

        // Segment 4: buf[cursor..end] in normal color
        // (the real text after cursor that was already typed — skip the part
        //  covered by inlineText since we just rendered it dim above)
        {
            int inlineLen = (int)hint.inlineText.size();
            int afterInline = m_cursor + inlineLen;
            if (afterInline < (int)m_buf.size())
                WriteNarrow(m_buf.substr(afterInline));
        }

        // Segment 5: appendText after end of buffer
        if (!hint.appendText.empty())
        {
            SetConsoleTextAttribute(m_hOut, hint.color);
            WriteNarrow(hint.appendText);
            SetConsoleTextAttribute(m_hOut, csbi.wAttributes);
        }

        // Park cursor at the edit position
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

    // ── Tab completion ────────────────────────────────────────────────────

    void DoCompletion(const std::string& prompt)
    {
        if (!m_completionCb) return;

        std::vector<std::string> candidates;
        m_completionCb(m_buf, candidates);
        if (candidates.empty()) return;

        if (candidates.size() == 1)
        {
            m_buf = candidates[0];
            m_cursor = (int)m_buf.size();
            Redraw(prompt);
            return;
        }

        // Extend to longest common prefix if possible
        std::string lcp = candidates[0];
        for (size_t i = 1; i < candidates.size(); ++i)
        {
            size_t j = 0;
            while (j < lcp.size() && j < candidates[i].size() && lcp[j] == candidates[i][j]) ++j;
            lcp = lcp.substr(0, j);
        }

        if (lcp.size() > m_buf.size())
        {
            m_buf = lcp;
            m_cursor = (int)m_buf.size();
            Redraw(prompt);
            return;
        }

        // Print candidate list on a new line, then redraw the prompt below it.
        // The new prompt row is one line below wherever we currently are.
        WriteConsoleW(m_hOut, L"\r\n", 2, nullptr, nullptr);
        for (const auto& c : candidates)
        {
            WriteNarrow(c);
            WriteConsoleW(m_hOut, L"  ", 2, nullptr, nullptr);
        }
        WriteConsoleW(m_hOut, L"\r\n", 2, nullptr, nullptr);

        // Update m_row to the new cursor row
        {
            CONSOLE_SCREEN_BUFFER_INFO csbi{};
            GetConsoleScreenBufferInfo(m_hOut, &csbi);
            m_row = csbi.dwCursorPosition.Y;
        }
        Redraw(prompt);
    }

    // ── Members ───────────────────────────────────────────────────────────

    HANDLE m_hIn, m_hOut;

    std::string m_buf;
    int         m_cursor = 0;
    SHORT       m_row = 0; // console row the prompt lives on

    int                     m_histMaxLen;
    int                     m_histIdx;
    std::deque<std::string> m_history;
    std::string             m_histScratch;

    WinCompletionCallback m_completionCb;
    WinHintCallback       m_hintCb;
};