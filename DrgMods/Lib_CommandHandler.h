#pragma once
// Lib_CommandHandler.h — CommandHandler.
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "Lib_Forward.h"
#include "Lib_Context.h"
#include "Lib_GameHooks.h"
#include "Lib_VarSystem.h"
#include "Common.h"
#include "StringLib.h"

extern ResponseBuffer* g_pRespBuffer;
extern void SendResponse(uint32_t cmdSeq, const std::string& msg);

// =========================================================================
// CommandHandler
// =========================================================================

class CommandHandler
{
public:
    using CommandFn = std::function<void(const CommandContext&)>;

    inline void Register(const std::string& name, CommandFn fn, std::string category, std::string description = "")
    {
        commands_[name] = { std::move(fn), std::move(category), std::move(description) };
    }

    inline bool Dispatch(const std::string& msg, uint32_t seq = 0) const
    {
        auto parts = Split(msg);
        if (parts.empty()) [[unlikely]] return false;

        if (parts[0] == "help")
        {
            PrintHelp(parts.size() > 1 ? parts[1] : "");
            SendResponse(seq, "ok");
            return true;
        }

        auto it = commands_.find(parts[0]);
        if (it == commands_.end()) [[unlikely]]
        {
            spdlog::warn("[CommandHandler] Unknown command: '{}'. Type 'help' for a list.", parts[0]);
            SendResponse(seq, "not ok");
            return false;
        }

        CommandFn fn = it->second.fn;
        GameHooks::ProcessEventHook::Get().Enqueue([fn, parts, seq]() mutable
            {
                // On game thread — safe to call VarSystem::Expand
                for (size_t i = 1; i < parts.size(); ++i)
                {
                    auto expanded = VarSystem::Expand(parts[i]);
                    if (!expanded.isValid) [[unlikely]] continue;
                    if (expanded.object)
                        parts[i] = expanded.object->GetName();
                    else if (!expanded.token.empty() ||
                        parts[i].substr(0, 4) == "var:" ||
                        parts[i].substr(0, 3) == "fn:")
                        parts[i] = expanded.token;
                }
                fn(CommandContext{ parts, seq });
                if (g_pRespBuffer && !g_pRespBuffer->ready.load(std::memory_order_acquire))
                    SendResponse(seq, "ok");
            });
        return true;
    }

private:
    struct CommandEntry { CommandFn fn; std::string category; std::string description; };
    std::unordered_map<std::string, CommandEntry> commands_;

    inline void PrintHelp(const std::string& filter) const
    {
        if (!filter.empty())
        {
            auto it = commands_.find(filter);
            if (it == commands_.end()) { spdlog::warn("[help] Unknown command: '{}'", filter); return; }
            const auto& e = it->second;
            spdlog::info("[help] [{}] {}: {}", e.category, it->first, e.description.empty() ? "(no description)" : e.description);
            return;
        }
        spdlog::info("[help] Available commands ({}):", commands_.size());
        std::map<std::string, std::vector<std::pair<std::string, const CommandEntry*>>> byCategory;
        for (const auto& [name, entry] : commands_)
            byCategory[entry.category].emplace_back(name, &entry);
        for (auto& [cat, entries] : byCategory)
        {
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            spdlog::info("[help] -- {} --", cat.empty() ? "Uncategorized" : cat);
            for (const auto& [name, entry] : entries)
                spdlog::info("[help]   {:20} {}", name, entry->description.empty() ? "(no description)" : entry->description);
        }
    }

    static constexpr bool IsSpace(char c) noexcept {
        return static_cast<unsigned char>(c) <= 32;
    }
    static constexpr size_t SkipWS(std::string_view s, size_t pos) noexcept {
        while (pos < s.size() && IsSpace(s[pos])) [[likely]] {
            pos++;
        }
        return pos;
    }
    static bool ExtractToken(std::string_view s, size_t& i, std::string& out_buf) {
        i = SkipWS(s, i);
        if (i >= s.size()) return false;

        out_buf.clear();
        if (s[i] == '"' || s[i] == '\'') [[unlikely]] {
            char q = s[i++];
            // Typical token size rarely exceeds 64, reserve to avoid multiple reallocs
            if (out_buf.capacity() < 64) out_buf.reserve(64);

            while (i < s.size() && s[i] != q) {
                if (s[i] == '\\' && i + 1 < s.size()) [[unlikely]] {
                    i++;
                    switch (s[i]) {
                        case 'n':  out_buf.push_back('\n'); break;
                        case 't':  out_buf.push_back('\t'); break;
                        case 'r':  out_buf.push_back('\r'); break;
                        case '0':  out_buf.push_back('\0'); break;
                        default:   out_buf.push_back(s[i]); break;
                    }
                    i++;
                }
                else {
                    out_buf.push_back(s[i++]);
                }
            }
            if (i < s.size()) i++; // skip closing quote
        }
        else {
            size_t start = i;
            while (i < s.size() && !IsSpace(s[i])) i++;
            out_buf.assign(s.substr(start, i - start));
        }
        return true;
    }
    inline static std::vector<std::string> Split(std::string_view s) {
        std::vector<std::string> out;
        // Optimization: Small strings usually have few tokens, larger ones more.
        out.reserve(s.size() / 10);

        size_t i = 0;
        std::string buffer;
        buffer.reserve(64); // Reuse memory for building tokens

        while (i < s.size()) {
            if (!ExtractToken(s, i, buffer)) break;

            // Handle Special Prefixes
            bool is_vec = (buffer == "vec:" || buffer == "rot:");
            bool is_var = !is_vec && (buffer == "fn:" || buffer == "name:" || buffer == "var:");

            if (is_vec) [[unlikely]] {
                std::string joined = std::move(buffer);
                std::string arg;
                for (int count = 0; count < 3; ++count) {
                    if (ExtractToken(s, i, arg)) {
                        if (joined.back() != ':') joined.push_back(',');
                        joined.append(arg);
                    }
                    else break;
                }
                out.push_back(std::move(joined));
                buffer.clear(); // Ensure buffer is ready for next iteration
                continue;
            }

            if (is_var) [[unlikely]] {
                std::string joined = std::move(buffer);
                std::string arg;
                if (ExtractToken(s, i, arg)) {
                    joined.append(arg);
                }
                out.push_back(std::move(joined));
                buffer.clear();
                continue;
            }

            // Normal token
            out.push_back(std::move(buffer));
        }

        return out;
    }
    public: 
        inline const std::unordered_map<std::string, CommandEntry>& GetEntries() const
        {
            return commands_;
        }
};
