// Lib_NetLogConfig.cpp — loads per-command skip lists from config.yaml.
// See Lib_NetLogConfig.h for the two-path search description.

#include "Lib_NetLogConfig.h"
#include "Lib_CommandHandler.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <filesystem>
#include <yaml-cpp/yaml.h>

namespace NetLogConfig
{
    // ── Two-path config search ────────────────────────────────────────────────
    // Uses the address of Load() to identify the containing DLL, so this works
    // correctly when compiled into both DrgMods.dll and RcMods.dll separately.

    static std::filesystem::path FindConfig()
    {
        wchar_t buf[MAX_PATH]{};
        HMODULE hm = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Load), &hm);
        if (hm) GetModuleFileNameW(hm, buf, MAX_PATH);
        const auto dir = std::filesystem::path(buf).parent_path();

        // 1. Next to the module DLL — distribution / in-game use
        auto p = dir / L"config.yaml";
        if (std::filesystem::exists(p)) return p;

        // 2. Two directories up — development (next to the solution file)
        std::error_code ec;
        p = std::filesystem::weakly_canonical(dir / L".." / L".." / L"config.yaml", ec);
        if (!ec && std::filesystem::exists(p)) return p;

        return {};
    }

    // ── Public API ────────────────────────────────────────────────────────────

    Config Load()
    {
        const auto path = FindConfig();
        if (path.empty()) return {};

        YAML::Node root;
        try { root = YAML::LoadFile(path.string()); }
        catch (const YAML::Exception&) { return {}; }

        Config cfg;

        auto ExtractSkip = [&](const char* section, std::vector<std::string>& out)
        {
            const auto sec = root[section];
            if (!sec || !sec.IsMap()) return;
            const auto skip = sec["skip"];
            if (!skip || !skip.IsSequence()) return;
            for (const auto& item : skip)
                if (item.IsScalar())
                    out.push_back(item.as<std::string>());
        };

        ExtractSkip("netlog", cfg.netSkip);

        // autorun section: list of commands to execute via RunConfig().
        // Each entry is either:
        //   - a scalar  → command name, no args (equivalent to dummyCtx)
        //   - a map     → { command: <name>, args: [arg0, arg1, ...] }
        const auto autorun = root["autorun"];
        if (autorun && autorun.IsSequence())
        {
            for (const auto& item : autorun)
            {
                if (item.IsScalar())
                {
                    cfg.autorun.push_back({ item.as<std::string>(), {} });
                }
                else if (item.IsMap())
                {
                    const auto cmd = item["command"];
                    if (!cmd || !cmd.IsScalar()) continue;
                    AutoRunEntry entry;
                    entry.command = cmd.as<std::string>();
                    const auto args = item["args"];
                    if (args && args.IsSequence())
                        for (const auto& arg : args)
                            if (arg.IsScalar()) entry.args.push_back(arg.as<std::string>());
                    cfg.autorun.push_back(std::move(entry));
                }
            }
        }

        return cfg;
    }

} // namespace NetLogConfig

// ── RunConfig ─────────────────────────────────────────────────────────────────

void RunConfig(CommandHandler& handler)
{
    const auto cfg = NetLogConfig::Load();
    if (cfg.autorun.empty())
    {
        info("[runcfg] No autorun entries in config.yaml");
        return;
    }
    info("[runcfg] Running {} autorun entr{}", cfg.autorun.size(), cfg.autorun.size() == 1 ? "y" : "ies");
    for (const auto& entry : cfg.autorun)
    {
        // Build a dispatch string: "command arg0 arg1 ..."
        // Quote args containing whitespace so CmdSplit reassembles them correctly.
        std::string msg = entry.command;
        for (const auto& arg : entry.args)
        {
            msg += ' ';
            if (arg.find_first_of(" \t") != std::string::npos)
                msg += '"' + arg + '"';
            else
                msg += arg;
        }
        info("[runcfg]   {}", msg);
        handler.Dispatch(msg);
    }
}
