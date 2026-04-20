#include <iostream>
#include <string>
#include <unordered_set>
#include <algorithm>

#include <Windows.h>

#include "../Drgmods/Common.h"
#include "../Drgmods/StringLib.h"

#include "WinInput.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Types
// ─────────────────────────────────────────────────────────────────────────────

struct KnownFunction
{
    std::string name;
    std::string params;
    std::string owner;
};

struct KnownCommand
{
    std::string name;
    std::string desc;   
    std::string params; 
};

// ─────────────────────────────────────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────────────────────────────────────

static CommandBuffer* g_pCmd = nullptr;
static std::vector<KnownFunction> g_KnownFunctions;
static std::vector<KnownCommand> g_KnownCommands;

static const std::vector<std::string> k_BuiltinCmds = {
    "load", "unload", "reload", "exit"
};

// ─────────────────────────────────────────────────────────────────────────────
//  Command helpers
// ─────────────────────────────────────────────────────────────────────────────

static void WaitForConsumed(CommandBuffer* pCmd)
{
    while (pCmd->hasCommand.load(std::memory_order_acquire))
        Sleep(10);
}

static uint32_t SendCommand(CommandBuffer* pCmd, const std::string& cmd)
{
    WaitForConsumed(pCmd);
    uint32_t seq = pCmd->seq.fetch_add(1, std::memory_order_relaxed) + 1;
    strncpy_s(pCmd->command, cmd.c_str(), sizeof(pCmd->command) - 1);
    pCmd->hasCommand.store(true, std::memory_order_release);
    return seq;
}

// ─────────────────────────────────────────────────────────────────────────────
//  WinInput callbacks
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
//  Completion / hint helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool IStartsWith(const std::string& str, const std::string& prefix)
{
    if (prefix.size() > str.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (tolower((unsigned char)str[i]) != tolower((unsigned char)prefix[i])) return false;
    return true;
}

static bool IEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

static std::string ExtractParams(const std::string& name, const std::string& desc)
{
    // Look for "... <name> <params>" pattern in desc (case-insensitive)
    std::string lower_desc = desc;
    std::string lower_name = name;
    for (auto& c : lower_desc) c = tolower((unsigned char)c);
    for (auto& c : lower_name) c = tolower((unsigned char)c);

    size_t pos = lower_desc.find(lower_name);
    if (pos == std::string::npos) return "";          // no match → no params
    size_t after = pos + name.size();
    if (after >= desc.size()) return "";              // name was at the end
    if (desc[after] != ' ') return "";
    return desc.substr(after + 1);                    // everything after "cmdname "
}

// Parse the buffer for the 'call' command.
//
// Commit convention: the FIRST "::" token after "call" is the func/args
// delimiter.  Everything between "call " and " ::" is the function name
// (may contain spaces).  Everything after "::" is arguments.
//
// Without "::" the user is still typing the function name.
// Tab-completing an exact match appends " :: " to commit it.
//
struct CallParse
{
    enum class Mode { TopLevel, FuncName, Args } mode = Mode::TopLevel;

    std::string funcPrefix;    // partial func name being typed  (FuncName mode)
    std::string funcName;      // committed func name            (Args mode)
    int         argIndex = 0;  // 0-based index of arg being typed
    std::string argPrefix;     // partial text of that argument

    std::string beforeToken;   // buf prefix before the current token
    std::string currentToken;  // the token the cursor is on
};

static CallParse ParseBuffer(const std::string& buf)
{
    CallParse p;

    // ── tokenise ──────────────────────────────────────────────────────────
    std::vector<std::string> words;
    std::vector<size_t>      starts;
    {
        size_t i = 0;
        while (i <= buf.size())
        {
            size_t sp = buf.find(' ', i);
            if (sp == std::string::npos) sp = buf.size();
            words.push_back(buf.substr(i, sp - i));
            starts.push_back(i);
            i = sp + 1;
            if (sp == buf.size()) break;
        }
    }

    if (words.empty()) { p.mode = CallParse::Mode::TopLevel; return p; }

    // ── top-level (first token) ───────────────────────────────────────────
    if (words.size() == 1 || words[0] != "call")
    {
        size_t last = words.size() - 1;
        p.mode = CallParse::Mode::TopLevel;
        p.currentToken = words[last];
        p.beforeToken = buf.substr(0, starts[last]);
        return p;
    }

    // ── "call ..." ────────────────────────────────────────────────────────
    // Find first "::" token after "call"
    size_t sepIdx = std::string::npos;
    for (size_t i = 1; i < words.size(); ++i)
        if (words[i] == "::") { sepIdx = i; break; }

    if (sepIdx == std::string::npos)
    {
        // No "::" yet — user is still typing the function name.
        // The function name is everything after "call ".
        p.mode = CallParse::Mode::FuncName;
        p.funcPrefix = buf.substr(starts[1]);   // preserves spaces in name
        p.currentToken = p.funcPrefix;
        p.beforeToken = buf.substr(0, starts[1]);
    }
    else
    {
        // "::" found — function name is words[1..sepIdx-1], args are after
        std::string fnPart;
        for (size_t i = 1; i < sepIdx; ++i)
        {
            if (i > 1) fnPart += ' ';
            fnPart += words[i];
        }

        p.mode = CallParse::Mode::Args;
        p.funcName = fnPart;

        size_t last = words.size() - 1;
        // argIndex = how many words come after "::" before the last word
        p.argIndex = std::max(0, (int)(last - sepIdx) - 1);
        // if cursor is right after "::" with nothing typed, last==sepIdx
        if (last == sepIdx)
        {
            p.argIndex = 0;
            p.argPrefix = "";
            p.currentToken = "";
            p.beforeToken = buf; // nothing to complete
        }
        else
        {
            p.argPrefix = words[last];
            p.currentToken = words[last];
            p.beforeToken = buf.substr(0, starts[last]);
        }
    }

    return p;
}

// Split a params string "Type1 name1, Type2 name2" into individual param strings
static std::vector<std::string> SplitParams(const std::string& params)
{
    std::vector<std::string> result;
    if (params.empty()) return result;
    size_t i = 0;
    while (i < params.size())
    {
        size_t comma = params.find(',', i);
        if (comma == std::string::npos) comma = params.size();
        std::string p = params.substr(i, comma - i);
        // trim
        while (!p.empty() && p.front() == ' ') p = p.substr(1);
        while (!p.empty() && p.back() == ' ') p.pop_back();
        if (!p.empty()) result.push_back(p);
        i = comma + 1;
    }
    return result;
}

static void CompletionCallback(const std::string& buf, std::vector<std::string>& out)
{
    auto p = ParseBuffer(buf);

    if (p.mode == CallParse::Mode::TopLevel)
    {
        // Only complete builtin commands at top level — functions need "call" prefix
        for (const auto& cmd : k_BuiltinCmds)
            if (IStartsWith(cmd, p.currentToken))
                out.push_back(p.beforeToken + cmd);

        for (const auto& kc : g_KnownCommands)
            if (IStartsWith(kc.name, p.currentToken))
                out.push_back(p.beforeToken + kc.name);
    }
    else if (p.mode == CallParse::Mode::FuncName)
    {
        for (const auto& fn : g_KnownFunctions)
        {
            if (!IStartsWith(fn.name, p.funcPrefix)) continue;

            if (IEquals(fn.name, p.funcPrefix))
            {
                // Exact match — commit with " :: " so user moves to args mode
                out.push_back(p.beforeToken + fn.name + " :: ");
            }
            else
            {
                out.push_back(p.beforeToken + fn.name);
            }
        }
    }
    // Mode::Args — no completion for argument values
}

static constexpr WORD kHintColor = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; // dim gray

static WinHint HintCallback(const std::string& buf, int cursor)
{
    if (buf.empty() || cursor < 0) return {};

    const std::string atCursor = buf.substr(0, cursor);
    auto p = ParseBuffer(atCursor);

    // Build a WinHint for a name match.
    // prefixLen = how many chars of fullName the user has typed at cursor.
    // The key insight: even if prefixLen == fullName.size() (full name typed,
    // cursor moved back), we still want appendText to show params/owner.
    // For inlineText we look at what's in buf ahead of cursor and dim those
    // chars if they case-insensitively match the name remainder.
    auto makeNameHint = [&](const std::string& fullName,
        size_t             prefixLen,
        const std::string& params,
        const std::string& owner,
        size_t             matchCount) -> WinHint
        {
            WinHint h;
            h.color = kHintColor;

            // nameRemainder: what comes after the typed prefix in the full name
            std::string nameRemainder = (prefixLen < fullName.size())
                ? fullName.substr(prefixLen) : "";

            int ahead = (int)buf.size() - cursor;
            if (ahead <= 0)
            {
                // Cursor at end of buffer — pure ghost text
                h.inlineText = nameRemainder;
            }
            else if (!nameRemainder.empty())
            {
                // Cursor mid-buffer — dim chars ahead that match nameRemainder
                // Use case-insensitive compare so "server_c" typed → "Server_C..." dims correctly
                int overlap = std::min((int)nameRemainder.size(), ahead);
                bool matches = true;
                for (int i = 0; i < overlap && matches; ++i)
                    if (tolower((unsigned char)buf[cursor + i]) !=
                        tolower((unsigned char)nameRemainder[i])) matches = false;
                if (matches) h.inlineText = nameRemainder.substr(0, overlap);
            }
            // nameRemainder empty = full name already typed; inlineText stays empty,
            // but appendText still shows params/owner below.

            if (matchCount == 1)
            {
                if (!params.empty()) h.appendText += "  (" + params + ")";
                if (!owner.empty())  h.appendText += "  [" + owner + "]";
            }
            else
            {
                h.appendText = "  +" + std::to_string(matchCount - 1) + " more";
            }
            return h;
        };

    // ── Top-level: only builtin commands, NOT scanned functions ───────────
    // Functions require "call" prefix — don't pollute top-level hints.
    if (p.mode == CallParse::Mode::TopLevel && !p.currentToken.empty())
    {
        // Gather builtin matches
        std::vector<std::string> builtinMatches;
        for (const auto& cmd : k_BuiltinCmds)
            if (IStartsWith(cmd, p.currentToken)) builtinMatches.push_back(cmd);

        // ← NEW: gather registered command matches
        std::vector<const KnownCommand*> cmdMatches;
        for (const auto& kc : g_KnownCommands)
            if (IStartsWith(kc.name, p.currentToken)) cmdMatches.push_back(&kc);

        if (!builtinMatches.empty())
            return makeNameHint(builtinMatches[0], p.currentToken.size(), "", "", builtinMatches.size());

        if (!cmdMatches.empty())
            return makeNameHint(cmdMatches[0]->name, p.currentToken.size(),
                cmdMatches[0]->params, "", cmdMatches.size());

        return {};
    }

    // ── FuncName inside "call" ────────────────────────────────────────────
    if (p.mode == CallParse::Mode::FuncName && !p.funcPrefix.empty())
    {
        std::vector<const KnownFunction*> matches;
        for (const auto& fn : g_KnownFunctions)
            if (IStartsWith(fn.name, p.funcPrefix)) matches.push_back(&fn);
        if (matches.empty()) return {};
        return makeNameHint(matches[0]->name, p.funcPrefix.size(),
            matches[0]->params, matches[0]->owner, matches.size());
    }

    // ── Args: param signature hint (append only, no inline) ───────────────
    if (p.mode == CallParse::Mode::Args)
    {
        const KnownFunction* fn = nullptr;
        for (const auto& kf : g_KnownFunctions)
            if (IEquals(kf.name, p.funcName)) { fn = &kf; break; }
        if (!fn || fn->params.empty()) return {};

        auto paramList = SplitParams(fn->params);
        if (p.argIndex >= (int)paramList.size()) return {};

        WinHint h;
        h.color = kHintColor;
        for (int i = p.argIndex; i < (int)paramList.size(); ++i)
        {
            if (i > p.argIndex) h.appendText += "  ";
            h.appendText += (i == p.argIndex)
                ? "[" + paramList[i] + "]"
                : paramList[i];
        }
        return h;
    }

    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
//  Response handling
//  ConsumeResponse() is safe to call while Readline() is blocking —
//  it only touches shared memory, no console output.
//  PrintPending() is called after Readline() returns.
// ─────────────────────────────────────────────────────────────────────────────

struct PendingOutput
{
    std::string text;   // non-empty = print this line
    bool        isScan; // true = also update g_KnownFunctions
    uint32_t    scanCount;
};

static PendingOutput ConsumeResponse(ResponseBuffer* pResp, HANDLE hRespEvent)
{
    PendingOutput out{};

    if (pResp->type == ResponseType::Scan)
    {
        const auto& sr = pResp->data.scan;
        g_KnownFunctions.clear();
        for (uint32_t i = 0; i < sr.count; ++i)
            g_KnownFunctions.push_back({ sr.funcs[i].name, sr.funcs[i].params, sr.funcs[i].owner });
        out.isScan = true;
        out.scanCount = sr.count;
        out.text = "Scanned " + std::to_string(sr.count) + " functions.";
    }
    else if (pResp->type == ResponseType::Commands)
    {
        const auto& cr = pResp->data.commands;
        g_KnownCommands.clear();
        for (uint32_t i = 0; i < cr.count; ++i)
        {
            KnownCommand kc;
            kc.name = cr.cmds[i].name;
            kc.desc = cr.cmds[i].desc;

            // Extract param hint: everything after "<name> " in the description
            // e.g. "Teleport: tp <x> <y> <z> [--rel] [--sweep]"
            //   or "Toggle god mode for local player"  (no params)
            size_t sp = kc.desc.find(' ');
            kc.params = (sp != std::string::npos) ? kc.desc.substr(sp + 1) : "";

            g_KnownCommands.push_back(std::move(kc));
        }
        out.isScan = false;
        out.text = "Loaded " + std::to_string(cr.count) + " commands.";
    }

    else
    {
        const char* text = pResp->data.text;
        // Suppress bare "ok" — the DLL log already says what happened
        if (text[0] != '\0' && !(text[0] == 'o' && text[1] == 'k' && text[2] == '\0'))
            out.text = text;
    }

    pResp->ready.store(false, std::memory_order_release);
    ResetEvent(hRespEvent);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Ctrl handler
// ─────────────────────────────────────────────────────────────────────────────

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType)
    {
    case CTRL_CLOSE_EVENT:
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        if (g_pCmd) SendCommand(g_pCmd, "unload");
        Sleep(200);
        return FALSE;
    default:
        return FALSE;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    system("chcp 65001 > nul");
    std::cout <<
        "--- DRG Mod CLI ---\n"
        "Commands:\n"
        "  load          - Inject DLL if not already loaded\n"
        "  unload        - Unload DLL\n"
        "  reload        - Unload and reload DLL\n"
        "  <text>        - Send custom command to DLL\n"
        "  exit          - Quit CLI\n\n"
        "  Tab           - Autocomplete\n"
        "  Right arrow   - Accept hint\n"
        "  Up/Down       - History\n\n";

    // ── Shared memory ──────────────────────────────────────────────────────

    HANDLE hCmdMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SHMEM_CMD);
    if (!hCmdMapping)
    {
        std::cout << "Failed to open command buffer. Is injector running?\n";
        return 1;
    }

    auto* pCmd = static_cast<CommandBuffer*>(
        MapViewOfFile(hCmdMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!pCmd)
    {
        std::cout << "Failed to map command buffer.\n";
        CloseHandle(hCmdMapping);
        return 1;
    }

    g_pCmd = pCmd;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    HANDLE hRespMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SHMEM_RESPONSE);
    if (!hRespMapping)
        std::cout << "Failed to open response buffer. Running in fire-and-forget mode.\n";

    auto* pResp = hRespMapping
        ? static_cast<ResponseBuffer*>(MapViewOfFile(hRespMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0))
        : nullptr;

    // Need EVENT_MODIFY_STATE so we can call ResetEvent on it
    HANDLE hRespEvent = hRespMapping
        ? OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, EVENT_RESP_READY)
        : nullptr;

    std::cout << "Connected.\n";

    // ── WinInput setup ─────────────────────────────────────────────────────

    WinInput input;
    input.SetCompletionCallback(CompletionCallback);
    input.SetHintCallback(HintCallback);
    input.SetHistoryMaxLen(200);

    // ── REPL ───────────────────────────────────────────────────────────────

    static const std::unordered_set<std::string, StringLib::CaseInsensitiveHash, StringLib::CaseInsensitiveEqual> k_NoReply = { "load", "unload", "reload","exit"};

    while (true)
    {
        std::string line;
        if (!input.Readline("> ", line))
            break;

        if (line.empty()) continue;

        input.HistoryAdd(line);

        if (_strcmpi(line.c_str(),"exit") == 0)
        {
            SendCommand(pCmd, "unload");
            Sleep(200);
            break;
        }

        uint32_t mySeq = SendCommand(pCmd, line);

        std::string cmdName = line.substr(0, line.find(' '));
        bool shouldWait = pResp && hRespEvent && !k_NoReply.contains(cmdName);

        if (!shouldWait)
        {
            std::cout << "Sent.\n";
            continue;
        }

        constexpr DWORD kTimeoutMs = 5000;
        DWORD64 deadline = GetTickCount64() + kTimeoutMs;
        PendingOutput pending{};
        bool gotResponse = false;

        while (GetTickCount64() < deadline)
        {
            // Check ready first — signal may have already fired before we entered the loop
            if (pResp->ready.load(std::memory_order_acquire) &&
                pResp->seq.load(std::memory_order_acquire) == mySeq)
            {
                // Only touches shared memory — safe while Readline owns the console
                pending = ConsumeResponse(pResp, hRespEvent);
                gotResponse = true;
                break;
            }

            DWORD64 remaining = deadline - GetTickCount64();
            WaitForSingleObject(hRespEvent, static_cast<DWORD>(remaining < 50 ? remaining : 50));
        }

        // Readline has returned by this point — safe to write to console
        if (!gotResponse)
            std::cout << "(no response within " << kTimeoutMs << "ms)\n";
        else if (!pending.text.empty())
            std::cout << pending.text << "\n";
    }

    // ── Cleanup ────────────────────────────────────────────────────────────

    if (pResp)        UnmapViewOfFile(pResp);
    if (hRespMapping) CloseHandle(hRespMapping);
    if (hRespEvent)   CloseHandle(hRespEvent);
    UnmapViewOfFile(pCmd);
    CloseHandle(hCmdMapping);
    return 0;
}