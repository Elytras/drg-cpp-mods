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

    // ── Filter API (call while holding Mutex()) ───────────────────────────
    void SetFilterUnderLock(const std::string& str);   // live-filter log pane
    void ClearFilterUnderLock();                        // remove filter, show all
    bool IsFilterActive()          const { return m_filterActive; }
    const std::string& FilterStr() const { return m_filterStr; }

    // ── Selection API (call while holding Mutex()) ────────────────────────
    // rectMode = true → rectangular (box) selection; false → linear (rows)
    void OnMouseDownUnderLock(COORD pos, bool rectMode);
    void OnMouseDragUnderLock(COORD pos);
    void OnMouseUpUnderLock(COORD pos);
    // Build clipboard text for the current selection, applying the current
    // filter.  Returns "" when nothing is selected.
    std::string CopySelectionUnderLock() const;
    void ClearSelectionUnderLock();

private:
    // ── Layout ────────────────────────────────────────────────────────────
    void  RecalcLayout();
    SHORT ConsoleWidth() const;
    COORD GetCursorPos() const;

    // ── CHAR_INFO buffer helpers ──────────────────────────────────────────
    static void FillLineInto(std::vector<CHAR_INFO>& buf, SHORT w, int row,
                              const std::string& utf8, WORD attr);
    // tag — centred UTF-8 label; empty → plain dash line
    static void FillDividerInto(std::vector<CHAR_INFO>& buf, SHORT w, int row,
                                 WORD attr, const std::string& tag);

    // Builds the centred tag for the log divider from current filter/scroll state.
    std::string BuildLogDivTag() const;

    // ── Selection helpers ─────────────────────────────────────────────────
    struct SelCoord {
        int  historyIdx = -1;  // index into m_logHistory; -1 = invalid
        int  col        = 0;   // screen column (used in rect mode)
        bool valid() const { return historyIdx >= 0; }
    };
    // Map a screen COORD (absolute row, absolute col) → history-relative coord.
    // Clamps to the nearest valid log-pane line if the click is outside.
    SelCoord ScreenToHistory(COORD pos) const;
    // Overlay the selection highlight onto a CHAR_INFO buffer that covers the
    // log pane (rows 0..m_logDivRow-1, width w).
    void ApplySelectionToBuffer(std::vector<CHAR_INFO>& buf, SHORT w) const;

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

    // ── Filter state ──────────────────────────────────────────────────────
    bool                m_filterActive  = false;
    std::string         m_filterStr;
    std::vector<size_t> m_filteredIndices;  // indices into m_logHistory

    // ── Selection state ───────────────────────────────────────────────────
    bool     m_selActive  = false; // LMB currently held
    bool     m_selDone    = false; // selection completed (LMB released)
    bool     m_selRect    = false; // true = rectangular, false = linear (full rows)
    SelCoord m_selAnchor;          // fixed corner (mousedown position)
    SelCoord m_selCurrent;         // moving corner (current mouse position)
};
