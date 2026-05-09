#pragma once
#include <vector>
#include <string>

struct CommandContext
{
    using uint32 = unsigned int;
    const std::vector<std::string>& args;
    uint32 seq = 0;

    size_t             ArgCount() const { return args.size(); }
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

    size_t             ArgCount() const { return args.size(); }
    inline const std::string& Arg(size_t i, const std::string& fallback = "") const
    {
        return i < args.size() ? args[i] : fallback;
    }

    MutableContext(std::vector<std::string>& a) : args(a) {}
    MutableContext(std::vector<std::string>&& a) : args(std::move(a)) {}
    MutableContext(const CommandContext& ctx) : args(ctx.args), seq(ctx.seq) {}
};
