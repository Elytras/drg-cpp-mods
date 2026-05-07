#pragma once
// Completion.h — REPL tab-completion and ghost-hint logic for the 'call' command.
// Included once by DrgCli.cpp after all globals are declared.

#include <algorithm>
#include <cctype>

#include "CliTypes.h"

// ─────────────────────────────────────────────────────────────────────────────
//  String helpers
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

// ─────────────────────────────────────────────────────────────────────────────
//  Buffer parser
//  Commit convention: the FIRST "::" token after "call" is the func/args delimiter.
// ─────────────────────────────────────────────────────────────────────────────

struct CallParse
{
    enum class Mode { TopLevel, FuncName, Args } mode = Mode::TopLevel;

    std::string funcPrefix;   // partial func name (FuncName mode)
    std::string funcName;     // committed func name (Args mode)
    int         argIndex = 0;
    std::string argPrefix;
    std::string beforeToken;
    std::string currentToken;
};

// Strips a single surrounding quote pair from a function prefix typed by the user.
// e.g. `"Fix Wid` → {"Fix Wid", '"'},  `Fix` → {"Fix", 0}
static std::pair<std::string, char> StripFuncQuotes(const std::string& s)
{
    if (s.empty()) return { s, 0 };
    char q = s.front();
    if (q != '"' && q != '\'') return { s, 0 };
    std::string stripped = s.substr(1);
    if (!stripped.empty() && stripped.back() == q)
        stripped.pop_back();
    return { stripped, q };
}

// Splits on the LAST "::" to separate an optional owner-filter prefix from the name part.
// "BP_Player_C::Fix"                → {"BP_Player_C",             "Fix"}
// "BP_Player_C::BP_Player_0::Fix W" → {"BP_Player_C::BP_Player_0", "Fix W"}
// "FixWidgets"                       → {"",                         "FixWidgets"}
static std::pair<std::string, std::string> SplitOwnerPrefix(const std::string& s)
{
    size_t pos = s.rfind("::");
    if (pos == std::string::npos) return { "", s };
    return { s.substr(0, pos), s.substr(pos + 2) };
}

// Checks whether fn matches an ownerPart::namePart query.
// ownerPart may be "Class::Instance" — only its first segment is compared against fn.owner.
static bool MatchesFunction(const KnownFunction& fn,
    const std::string& ownerPart, const std::string& namePart)
{
    if (!IStartsWith(fn.name, namePart)) return false;
    if (ownerPart.empty()) return true;
    size_t colonPos = ownerPart.find("::");
    const std::string classFilter = (colonPos == std::string::npos)
        ? ownerPart : ownerPart.substr(0, colonPos);
    return IStartsWith(fn.owner, classFilter);
}

static CallParse ParseBuffer(const std::string& buf)
{
    CallParse p;

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

    if (words.size() == 1 || words[0] != "call")
    {
        size_t last = words.size() - 1;
        p.mode = CallParse::Mode::TopLevel;
        p.currentToken = words[last];
        p.beforeToken = buf.substr(0, starts[last]);
        return p;
    }

    size_t sepIdx = std::string::npos;
    for (size_t i = 1; i < words.size(); ++i)
        if (words[i] == "::") { sepIdx = i; break; }

    if (sepIdx == std::string::npos)
    {
        p.mode = CallParse::Mode::FuncName;
        p.funcPrefix = buf.substr(starts[1]);
        p.currentToken = p.funcPrefix;
        p.beforeToken = buf.substr(0, starts[1]);
    }
    else
    {
        std::string fnPart;
        for (size_t i = 1; i < sepIdx; ++i)
        {
            if (i > 1) fnPart += ' ';
            fnPart += words[i];
        }

        p.mode = CallParse::Mode::Args;
        p.funcName = SplitOwnerPrefix(StripFuncQuotes(fnPart).first).second;

        size_t last = words.size() - 1;
        p.argIndex = std::max(0, (int)(last - sepIdx) - 1);
        if (last == sepIdx)
        {
            p.argIndex = 0;
            p.currentToken = "";
            p.beforeToken = buf;
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
        while (!p.empty() && p.front() == ' ') p = p.substr(1);
        while (!p.empty() && p.back() == ' ') p.pop_back();
        if (!p.empty()) result.push_back(p);
        i = comma + 1;
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Completion callback
// ─────────────────────────────────────────────────────────────────────────────

static void CompletionCallback(const std::string& buf, std::vector<std::string>& out)
{
    auto p = ParseBuffer(buf);

    if (p.mode == CallParse::Mode::TopLevel)
    {
        for (const auto& cmd : k_BuiltinCmds)
            if (IStartsWith(cmd, p.currentToken))
                out.push_back(p.beforeToken + cmd);

        for (const auto& kc : g_KnownCommands)
            if (IStartsWith(kc.name, p.currentToken))
                out.push_back(p.beforeToken + kc.name);
    }
    else if (p.mode == CallParse::Mode::FuncName)
    {
        auto sq = StripFuncQuotes(p.funcPrefix);
        const std::string& stripped = sq.first;
        const char quoteChar = sq.second;
        auto op = SplitOwnerPrefix(stripped);
        const std::string& ownerPart = op.first;
        const std::string& namePart = op.second;
        for (const auto& fn : g_KnownFunctions)
        {
            if (!MatchesFunction(fn, ownerPart, namePart)) continue;
            std::string fullRef = ownerPart.empty() ? fn.name : ownerPart + "::" + fn.name;
            std::string quoted = quoteChar
                ? std::string(1, quoteChar) + fullRef + quoteChar
                : fullRef;
            if (IEquals(fn.name, namePart))
                out.push_back(p.beforeToken + quoted + " :: ");
            else
                out.push_back(p.beforeToken + quoted);
        }
    }
    // Mode::Args — no completion for argument values
}

// ─────────────────────────────────────────────────────────────────────────────
//  Hint callback
// ─────────────────────────────────────────────────────────────────────────────

static constexpr WORD kHintColor = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

static WinHint HintCallback(const std::string& buf, int cursor)
{
    if (buf.empty() || cursor < 0) return {};

    const std::string atCursor = buf.substr(0, cursor);
    auto p = ParseBuffer(atCursor);

    auto makeNameHint = [&](const std::string& fullName,
        size_t             prefixLen,
        const std::string& params,
        const std::string& owner,
        size_t             matchCount) -> WinHint
        {
            WinHint h;
            h.color = kHintColor;

            std::string nameRemainder = (prefixLen < fullName.size())
                ? fullName.substr(prefixLen) : "";

            int ahead = (int)buf.size() - cursor;
            if (ahead <= 0)
            {
                h.inlineText = nameRemainder;
            }
            else if (!nameRemainder.empty())
            {
                int overlap = std::min((int)nameRemainder.size(), ahead);
                bool matches = true;
                for (int i = 0; i < overlap && matches; ++i)
                    if (tolower((unsigned char)buf[cursor + i]) !=
                        tolower((unsigned char)nameRemainder[i])) matches = false;
                if (matches) h.inlineText = nameRemainder.substr(0, overlap);
            }

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

    if (p.mode == CallParse::Mode::TopLevel && !p.currentToken.empty())
    {
        std::vector<std::string> builtinMatches;
        for (const auto& cmd : k_BuiltinCmds)
            if (IStartsWith(cmd, p.currentToken)) builtinMatches.push_back(cmd);

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

    if (p.mode == CallParse::Mode::FuncName && !p.funcPrefix.empty())
    {
        auto sq = StripFuncQuotes(p.funcPrefix);
        const std::string& stripped = sq.first;
        auto op = SplitOwnerPrefix(stripped);
        const std::string& ownerPart = op.first;
        const std::string& namePart = op.second;
        std::vector<const KnownFunction*> matches;
        for (const auto& fn : g_KnownFunctions)
            if (MatchesFunction(fn, ownerPart, namePart)) matches.push_back(&fn);
        if (matches.empty()) return {};
        return makeNameHint(matches[0]->name, namePart.size(),
            matches[0]->params, matches[0]->owner, matches.size());
    }

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