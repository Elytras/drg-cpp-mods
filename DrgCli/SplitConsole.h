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
//  Thread safety:
//    PrintLog / PrintCmd / ScrollLog  — acquire mutex themselves (external threads)
//    *UnderLock variants              — caller already holds Mutex() (WinInput callbacks)

#include <Windows.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class SplitConsole
{
    static constexpr int    kCliH   = 3;
    static constexpr size_t kLogMax = 100'000;
    static constexpr int    kCmdMax = 200;

public:
    SplitConsole();

    // ── Public API ────────────────────────────────────────────────────────

    void PrintLog(const std::string& line);
    void PrintLogUnderLock(const std::string& line);

    void PrintCmd(const std::string& line);
    void PrintCmdUnderLock(const std::string& line);

    void ScrollLog(int delta);
    void ScrollLogUnderLock(int delta);

    SHORT      InputRow()  const { return m_inputRow; }
    SHORT      LogDivRow() const { return m_logDivRow; }
    std::mutex& Mutex()          { return m_mutex; }

    void  HandleResizeLocked();
    SHORT SetAcHeightUnderLock(int acH);
    void  ClearAcUnderLock();

private:
    // ── Layout ────────────────────────────────────────────────────────────
    void  RecalcLayout();
    SHORT ConsoleWidth() const;
    COORD GetCursorPos() const;

    // ── CHAR_INFO buffer helpers ──────────────────────────────────────────
    static void FillLineInto(std::vector<CHAR_INFO>& buf, SHORT w, int row,
                              const std::string& utf8, WORD attr);
    static void FillDividerInto(std::vector<CHAR_INFO>& buf, SHORT w, int row,
                                 WORD attr, int scrollIndicator);

    // ── Rendering ─────────────────────────────────────────────────────────
    void RenderAll();
    void RenderLogPane();
    void RenderCliPane();
    void DrawLogDivider();
    void DrawCliDivider();

    // ── Core locked operations ────────────────────────────────────────────
    void PrintLogLocked(const std::string& line);
    void PrintCmdLocked(const std::string& line);
    void ScrollLogLocked(int delta);

    // ── Members ───────────────────────────────────────────────────────────
    HANDLE     m_hOut;
    std::mutex m_mutex;

    int   m_acH;
    SHORT m_consH;
    SHORT m_logDivRow;
    SHORT m_cliDivRow;
    SHORT m_inputRow;
    int   m_scrollOffset;

    std::deque<std::string> m_logHistory;
    std::deque<std::string> m_cliHistory;
};
