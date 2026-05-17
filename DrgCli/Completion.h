#pragma once
// Completion.h — REPL tab-completion and ghost-hint logic for the 'call' command.

#include <string>
#include <vector>

#include "CliTypes.h"
#include "WinInput.h"

// ── Buffer parser ─────────────────────────────────────────────────────────────

struct CallParse
{
    enum class Mode { TopLevel, FuncName, Args } mode = Mode::TopLevel;

    std::string funcPrefix;
    std::string funcName;
    int         argIndex = 0;
    std::string argPrefix;
    std::string beforeToken;
    std::string currentToken;
};

// ── String helpers ────────────────────────────────────────────────────────────

bool IStartsWith(const std::string& str, const std::string& prefix);
bool IEquals(const std::string& a, const std::string& b);

// ── Parse helpers ─────────────────────────────────────────────────────────────

std::pair<std::string, char>        StripFuncQuotes(const std::string& s);
std::pair<std::string, std::string> SplitOwnerPrefix(const std::string& s);
bool MatchesFunction(const KnownFunction& fn,
                     const std::string& ownerPart, const std::string& namePart);
CallParse           ParseBuffer(const std::string& buf);
std::vector<std::string> SplitParams(const std::string& params);

// ── WinInput callbacks ────────────────────────────────────────────────────────

void    CompletionCallback(const std::string& buf, std::vector<std::string>& out);
WinHint HintCallback(const std::string& buf, int cursor);
