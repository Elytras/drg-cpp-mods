// Aimbot_Config.cpp — per-weapon override storage, JSON loading, and resolve helpers.
//
// Compile-time tunables (FOV cones, sampling constants, keybinds) and the
// Debug flags are inline constexpr values in Aim_Internal.h::Config / ::Debug.
// Go there to change constants without touching function logic.

#include "Aim_Internal.h"

#include <filesystem>
#include <fstream>
#include <mutex>

#include <nlohmann/json.hpp>

using namespace SDK;

namespace AimAssist {
namespace Config {

std::unordered_map<std::string, WeaponConfigOverride>& WeaponOverridesRef()
{
    static std::unordered_map<std::string, WeaponConfigOverride> map;
    return map;
}

// Parse aim_config.json. Unknown keys are ignored; bad types are skipped with a warning.
static void LoadOverridesFromJSON(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    try
    {
        std::ifstream ifs(path);
        nlohmann::json j;
        ifs >> j;
        if (!j.contains("weapons") || !j["weapons"].is_object())
        {
            warn("[aim config] {} missing top-level 'weapons' object", path.string());
            return;
        }
        auto& map = WeaponOverridesRef();
        int loaded = 0;
        for (auto it = j["weapons"].begin(); it != j["weapons"].end(); ++it)
        {
            const std::string& className = it.key();
            const auto& w = it.value();
            if (!w.is_object()) continue;

            WeaponConfigOverride& wc = map[className];
            if (auto k = w.find("AimbotFOVDeg");        k != w.end() && k->is_number())  wc.AimbotFOVDeg        = k->get<float>();
            if (auto k = w.find("SilentAimFOVDeg");     k != w.end() && k->is_number())  wc.SilentAimFOVDeg     = k->get<float>();
            if (auto k = w.find("SilentAimRequireLOS"); k != w.end() && k->is_boolean()) wc.SilentAimRequireLOS = k->get<bool>();

            auto parseList = [](const nlohmann::json& arr) {
                std::vector<FName> v;
                for (const auto& e : arr)
                    if (e.is_string())
                        v.push_back(FName(StringLib::ToWide(e.get<std::string>()).c_str()));
                return v;
            };
            if (auto k = w.find("IgnoreBaseClasses");   k != w.end() && k->is_array())  wc.IgnoreBaseClasses   = parseList(*k);
            if (auto k = w.find("ForceIncludeClasses"); k != w.end() && k->is_array())  wc.ForceIncludeClasses = parseList(*k);
            if (auto k = w.find("TargetSelector");      k != w.end() && k->is_string()) wc.TargetSelector      = k->get<std::string>();
            ++loaded;
        }
        info("[aim config] loaded {} weapon overrides from {}", loaded, path.string());
    }
    catch (const std::exception& e)
    {
        warn("[aim config] JSON load failed: {}", e.what());
    }
}

void EnsureOverridesLoaded()
{
    static std::once_flag once;
    std::call_once(once, []
    {
        // ── C++ hardcoded defaults — uncomment / extend as needed ────────────
        // auto& m = WeaponOverridesRef();
        // m["WPN_SMG_C"]    = { .SilentAimFOVDeg = 45.f };
        // m["WPN_Sniper_C"] = { .SilentAimFOVDeg = 10.f, .SilentAimRequireLOS = true };

        // ── JSON override (if file present alongside the DLL) ────────────────
        wchar_t modulePath[MAX_PATH]{};
        HMODULE hMod = GetModuleHandleW(L"DrgMods.dll");
        if (hMod && GetModuleFileNameW(hMod, modulePath, MAX_PATH))
        {
            std::filesystem::path p = modulePath;
            LoadOverridesFromJSON(p.parent_path() / "aim_config.json");
        }
    });
}

float ResolveAimbotFOV(AItem* eq)
{
    EnsureOverridesLoaded();
    if (eq && eq->Class)
    {
        auto& m = WeaponOverridesRef();
        if (auto it = m.find(eq->Class->Name.ToString()); it != m.end() && it->second.AimbotFOVDeg)
            return *it->second.AimbotFOVDeg;
    }
    return AimbotFOVDeg;
}

float ResolveSilentAimFOV(AItem* eq)
{
    EnsureOverridesLoaded();
    if (eq && eq->Class)
    {
        auto& m = WeaponOverridesRef();
        if (auto it = m.find(eq->Class->Name.ToString()); it != m.end() && it->second.SilentAimFOVDeg)
            return *it->second.SilentAimFOVDeg;
    }
    return SilentAimFOVDeg;
}

bool ResolveSilentAimRequireLOS(AItem* eq)
{
    EnsureOverridesLoaded();
    if (eq && eq->Class)
    {
        auto& m = WeaponOverridesRef();
        if (auto it = m.find(eq->Class->Name.ToString()); it != m.end() && it->second.SilentAimRequireLOS)
            return *it->second.SilentAimRequireLOS;
    }
    return SilentAimRequireLOS;
}

Targeting::SelectorFn ResolveTargetSelector(AItem* eq)
{
    EnsureOverridesLoaded();
    if (eq && eq->Class)
    {
        auto& m = WeaponOverridesRef();
        if (auto it = m.find(eq->Class->Name.ToString()); it != m.end() && it->second.TargetSelector)
            return Targeting::Get(*it->second.TargetSelector);
    }
    return Targeting::Get("default");
}

} // namespace Config
} // namespace AimAssist
