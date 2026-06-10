// Lib_NetLogConfig.cpp — loads per-command skip lists from config.yaml.
// See Lib_NetLogConfig.h for the two-path search description.

#include "Lib_NetLogConfig.h"
#include "Lib_CommandHandler.h"
#include "Saveable.h"          // VarSystem::Saveable<std::string> — the `autorun` path cvar
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace NetLogConfig
{
    // ── Two-path config search ────────────────────────────────────────────────
    // Uses the address of Load() to identify the containing DLL, so this works
    // correctly when compiled into both DrgMods.dll and RcMods.dll separately.

    static std::filesystem::path ModuleDir()
    {
        wchar_t buf[MAX_PATH]{};
        HMODULE hm = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Load), &hm);
        if (hm) GetModuleFileNameW(hm, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path();
    }

    // Two-path search for a config file by name: next to the DLL first
    // (distribution), then two directories up (development, next to the .sln).
    static std::filesystem::path FindFile(const wchar_t* name)
    {
        const auto dir = ModuleDir();

        auto p = dir / name;
        if (std::filesystem::exists(p)) return p;

        std::error_code ec;
        p = std::filesystem::weakly_canonical(dir / L".." / L".." / name, ec);
        if (!ec && std::filesystem::exists(p)) return p;

        return {};
    }

    // Resolve `name` via the two-path search, falling back to the preferred
    // (next-to-DLL) location so a UI can create the file on first save.
    static std::string ResolvePath(const wchar_t* name)
    {
        const auto found = FindFile(name);
        if (!found.empty()) return found.string();
        const auto dir = ModuleDir();
        if (dir.empty()) return {};
        return (dir / name).string();
    }

    std::string ConfigPath() { return ResolvePath(L"config.yaml"); }

    // The `autorun` cvar holds an explicit script path (Persistent, archived to
    // settings.json). Empty (the default) means "use the resolved autorun.cfg".
    // A non-empty value that names an existing file wins — this enables profiles
    // (point `autorun` at a different .cfg).
    static VarSystem::Saveable<std::string> g_autorunPath{ "autorun", std::string{} };

    std::string AutorunPath()
    {
        const std::string override = g_autorunPath.get();
        if (!override.empty() && std::filesystem::exists(override)) return override;
        return ResolvePath(L"autorun.cfg");
    }

    // ── Public API ────────────────────────────────────────────────────────────

    Config Load()
    {
        const auto path = FindFile(L"config.yaml");
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
        return cfg;
    }

} // namespace NetLogConfig

// ── RunConfig ─────────────────────────────────────────────────────────────────
// The autorun script is plain text: one console command per line, dispatched
// verbatim (same syntax as the overlay/CLI input). Blank lines and lines whose
// first non-space character starts '#' or '//' are comments. Supersedes the old
// YAML `autorun:` section — see autorun.cfg.

void RunConfig(CommandHandler& handler)
{
    const std::string path = NetLogConfig::AutorunPath();
    std::ifstream f(path);
    if (!f)
    {
        info("[runcfg] No autorun script at {}", path);
        return;
    }

    info("[runcfg] Running autorun script {}", path);
    std::string line;
    size_t n = 0;
    while (std::getline(f, line))
    {
        // Trim leading/trailing ASCII whitespace (incl. a trailing '\r' on CRLF files).
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;                 // blank
        const auto last = line.find_last_not_of(" \t\r\n");
        const std::string cmd = line.substr(first, last - first + 1);

        if (cmd[0] == '#' || cmd.rfind("//", 0) == 0) continue;   // comment

        info("[runcfg]   {}", cmd);
        handler.Dispatch(cmd);
        ++n;
    }

    if (n == 0) info("[runcfg] autorun script had no commands");
}
