#include "Lib_CommandHandler.h"
#include "Lib_GameHooks.h"
#include "Lib_VarSystem.h"
#include <string_view>
#include <algorithm>
#include <map>

// Default: no tap. OverlayConsole::Init installs one to mirror responses in-game.
std::function<void(const std::string&)> g_responseTap;
static constexpr bool CmdIsSpace(char c) noexcept
{
    return static_cast<unsigned char>(c) <= 32;
}

static constexpr size_t CmdSkipWS(std::string_view s, size_t pos) noexcept
{
    while (pos < s.size() && CmdIsSpace(s[pos]))
        ++pos;
    return pos;
}

static bool CmdExtractToken(std::string_view s, size_t& i, std::string& out)
{
    i = CmdSkipWS(s, i);
    if (i >= s.size()) return false;

    out.clear();
    if (s[i] == '"' || s[i] == '\'') [[unlikely]] {
        char q = s[i++];
        if (out.capacity() < 64) out.reserve(64);
        while (i < s.size() && s[i] != q) {
            if (s[i] == '\\' && i + 1 < s.size()) [[unlikely]] {
                ++i;
                switch (s[i]) {
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                case '0':  out.push_back('\0'); break;
                default:   out.push_back(s[i]); break;
                }
                ++i;
            }
            else out.push_back(s[i++]);
        }
        if (i < s.size()) ++i; // skip closing quote
    }
    else {
        size_t start = i;
        while (i < s.size() && !CmdIsSpace(s[i])) ++i;
        out.assign(s.substr(start, i - start));
    }
    return true;
}

static std::vector<std::string> CmdSplit(std::string_view s)
{
    std::vector<std::string> out;
    out.reserve(s.size() / 10);
    size_t i = 0;
    std::string buf;
    buf.reserve(64);

    while (i < s.size()) {
        if (!CmdExtractToken(s, i, buf)) break;

        bool is_vec = (buf == "vec:" || buf == "rot:");
        bool is_var = !is_vec && (buf == "fn:" || buf == "name:" || buf == "var:");

        if (is_vec) [[unlikely]] {
            std::string joined = std::move(buf);
            std::string arg;
            for (int n = 0; n < 3; ++n) {
                if (CmdExtractToken(s, i, arg)) {
                    if (joined.back() != ':') joined.push_back(',');
                    joined.append(arg);
                }
                else break;
            }
            out.push_back(std::move(joined));
            buf.clear();
            continue;
        }

        if (is_var) [[unlikely]] {
            std::string joined = std::move(buf);
            std::string arg;
            if (CmdExtractToken(s, i, arg)) joined.append(arg);
            out.push_back(std::move(joined));
            buf.clear();
            continue;
        }

        out.push_back(std::move(buf));
    }
    return out;
}

// ── CommandHandler ────────────────────────────────────────────────────────

void CommandHandler::Register(const std::string& name, CommandFn fn,
                              std::string category, std::string description)
{
    commands_[name] = { std::move(fn), std::move(category), std::move(description) };
}

bool CommandHandler::Dispatch(const std::string& msg, uint32_t seq) const
{
    auto parts = CmdSplit(msg);
    if (parts.empty()) [[unlikely]] return false;

    if (parts[0] == "help") {
        PrintHelp(parts.size() > 1 ? parts[1] : "");
        if (seq != 0) SendResponse(seq, "ok");
        return true;
    }

    auto it = commands_.find(parts[0]);
    if (it == commands_.end()) [[unlikely]] {
        warn("[CommandHandler] Unknown command: '{}'. Type 'help' for a list.", parts[0]);
        if (seq != 0) SendResponse(seq, "not ok");
        return false;
    }

    CommandFn fn = it->second.fn;
    GameHooks::ProcessEventHook::Get().Enqueue([fn, parts, seq]() mutable
        {
            for (size_t i = 1; i < parts.size(); ++i) {
                auto expanded = VarSystem::Expand(parts[i]);
                if (!expanded.isValid) [[unlikely]] {
                    warn("[CommandHandler] arg {} ('{}') failed to expand — passing through literally", i, parts[i]);
                    continue;
                }
                if (expanded.object)
                    parts[i] = expanded.object->GetName();
                else if (!expanded.token.empty() ||
                         parts[i].substr(0, 4) == "var:" ||
                         parts[i].substr(0, 3) == "fn:")
                    parts[i] = expanded.token;
            }
            fn(CommandContext{ parts, seq });
            // seq==0 means an internal/autorun dispatch — no CLI consumer is waiting,
            // so don't write a response.  Leaving a seq=0 "ok" in the buffer would
            // block the next real SendResponse (which spins up to 1 s waiting for
            // ready=false), causing listcmds to time out after DllReady fires.
            if (seq != 0 && g_pRespBuffer && !g_pRespBuffer->ready.load(std::memory_order_acquire))
                SendResponse(seq, "ok");
        });
    return true;
}

void CommandHandler::PrintHelp(const std::string& filter) const
{
    if (!filter.empty()) {
        auto it = commands_.find(filter);
        if (it == commands_.end()) { warn("[help] Unknown command: '{}'", filter); return; }
        const auto& e = it->second;
        info("[help] [{}] {}: {}", e.category, it->first,
            e.description.empty() ? "(no description)" : e.description);
        return;
    }
    info("[help] Available commands ({}):", commands_.size());
    std::map<std::string, std::vector<std::pair<std::string, const CommandEntry*>>> byCategory;
    for (const auto& [name, entry] : commands_)
        byCategory[entry.category].emplace_back(name, &entry);
    for (auto& [cat, entries] : byCategory) {
        std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        info("[help] -- {} --", cat.empty() ? "Uncategorized" : cat);
        for (const auto& [name, entry] : entries)
            info("[help]   {:20} {}", name,
                entry->description.empty() ? "(no description)" : entry->description);
    }
}

const std::unordered_map<std::string, CommandHandler::CommandEntry>&
CommandHandler::GetEntries() const
{
    return commands_;
}
