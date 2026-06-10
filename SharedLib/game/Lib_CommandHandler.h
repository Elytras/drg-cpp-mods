 #pragma once
// Lib_CommandHandler.h — CommandContext, MutableContext, CommandHandler.
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
// Plain includes — each consumer's AdditionalIncludeDirectories resolves
// these to the right game's headers (DrgMods/ for DrgMods builds, RcMods/
// for RcMods builds). Hardcoding "../DrgMods/" here breaks RcMods because
// it pulls in both games' Common.h / SDK in the same translation unit.
// The comment above is stale. but i cba editing it yet

#ifdef RogueCore // Im so done with intellisense failing to deduce types that are sdk dependant
#include "../../RcMods/Lib_Forward.h"
#include "../../RcMods/Common.h"
#else
#include "../../DrgMods/Lib_Forward.h"
#include "../../DrgMods/Common.h"
#endif
#include "../core/IpcProtocol.h"   // ResponseBuffer

// =========================================================================
// Command contexts  (was Lib_Context.h)
// =========================================================================

struct CommandContext
{
    using uint32 = unsigned int;
    const std::vector<std::string>& args;
    uint32 seq = 0;

    size_t ArgCount() const { return args.size(); }
    inline const std::string& Arg(size_t i, const std::string& fallback = "") const
    {
        return i < args.size() ? args[i] : fallback;
    }
};

struct MutableContext
{
    using uint32 = unsigned int;
    std::vector<std::string> args;
    uint32 seq = 0;

    size_t ArgCount() const { return args.size(); }
    inline const std::string& Arg(size_t i, const std::string& fallback = "") const
    {
        return i < args.size() ? args[i] : fallback;
    }

    MutableContext(std::vector<std::string>& a)        : args(a) {}
    MutableContext(std::vector<std::string>&& a)       : args(std::move(a)) {}
    MutableContext(const CommandContext& ctx)           : args(ctx.args), seq(ctx.seq) {}
};

extern ResponseBuffer* g_pRespBuffer;
extern void SendResponse(uint32_t cmdSeq, const std::string& msg);

// Optional sink that mirrors every text response into another consumer (the
// in-game overlay console). Set by OverlayConsole::Init; both games' SendResponse
// feed it so structured/scan/list dumps show in-game, not just over IPC. Runs on
// the game thread; the callee must be thread-safe and must not call SendResponse.
extern std::function<void(const std::string&)> g_responseTap;

// =========================================================================
// CommandHandler
// =========================================================================

class CommandHandler
{

public:
    using CommandFn = std::function<void(const CommandContext&)>;

    void Register(const std::string& name, CommandFn fn, std::string category, std::string description = "");
    bool Dispatch(const std::string& msg, uint32_t seq = 0) const;
private:
    struct CommandEntry { CommandFn fn; std::string category; std::string description; };
    std::unordered_map<std::string, CommandEntry> commands_;

    void PrintHelp(const std::string& filter) const;
public:
    const std::unordered_map<std::string, CommandEntry>& GetEntries() const;
};
