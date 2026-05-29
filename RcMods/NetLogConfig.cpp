#include "NetLogConfig.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <yaml-cpp/yaml.h>
#include <string>

namespace NetLogConfig
{
    // ── Path helper ───────────────────────────────────────────────────────────

    static std::wstring GetModuleDir()
    {
        wchar_t buf[MAX_PATH]{};
        HMODULE hm = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Load), &hm);
        GetModuleFileNameW(hm, buf, MAX_PATH);
        std::wstring path(buf);
        const auto slash = path.rfind(L'\\');
        return (slash != std::wstring::npos) ? path.substr(0, slash + 1) : path;
    }

    // ── Public API ────────────────────────────────────────────────────────────

    Config Load()
    {
        const std::wstring wpath = GetModuleDir() + L"config.yaml";

        // yaml-cpp only takes narrow paths; convert via Win32.
        char path[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path, MAX_PATH, nullptr, nullptr);

        YAML::Node root;
        try                            { root = YAML::LoadFile(path); }
        catch (const YAML::Exception&) { return {}; }  // file missing or malformed

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

        ExtractSkip("lognetclient", cfg.netClientSkip);
        ExtractSkip("lognetserver", cfg.netServerSkip);

        return cfg;
    }

} // namespace NetLogConfig
