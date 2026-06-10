// Aimbot_Config.cpp — global config loading (aimbot.yaml) + per-weapon override storage.
//
// Global tunables (FOV cones, sampling constants, keybinds, class lists) are
// loaded from aimbot.yaml, searched in two locations (first match wins):
//   1. Next to DrgMods.dll          — distribution / in-place use
//   2. Two directories above DLL   — development (next to Drgmods.sln)
//
// Per-weapon overrides live in the 'weapons' section of the same file,
// replacing the old aim_config.json approach.
//
// Debug flags (Debug::LogSilentAim etc.) remain inline constexpr in
// Aim_Internal.h — they drive if-constexpr branches and cannot be runtime.

#include "Aim_Internal.h"
#include "Saveable.h"   // VarSystem::Saveable<T> — scalar tunables are cvars now

#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <yaml-cpp/yaml.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

using namespace SDK;

namespace AimAssist {
namespace Config {

// ── Scalar tunables: cvars (persisted to settings.json) ─────────────────────────
// Formerly GlobalConfig fields parsed from aimbot.yaml's `globals:` section. Now
// VarSystem cvars: typed, persistent, editable live via `set aim.* …` / the overlay
// Vars tab / `get aim.*`. Read live (game-thread) by the Resolve*/accessors below —
// no snapshot rebuild, no reloadaimcfg needed for a value to take effect.
static VarSystem::Saveable<float> g_aimFov              { "aim.fov",                   0.f   };
static VarSystem::Saveable<float> g_silentFov           { "aim.silent_fov",            360.f };
static VarSystem::Saveable<float> g_bodyRadiusScale     { "aim.body_radius_scale",     0.75f };
static VarSystem::Saveable<float> g_bodyRadiusFallback  { "aim.body_radius_fallback",  15.f  };
static VarSystem::Saveable<float> g_gimbalFlipThreshold { "aim.gimbal_flip_threshold", 90.f  };
static VarSystem::Saveable<bool>  g_silentRequireLos    { "aim.silent_require_los",    false };

// ── Storage ───────────────────────────────────────────────────────────────────

// Globals + per-weapon overrides are bundled into one immutable snapshot built
// off to the side and swapped in atomically. Readers take a shared_ptr copy
// (lock-free after the first load); ReloadGlobals() builds a fresh snapshot and
// atomically replaces the pointer, so in-flight readers keep seeing their
// consistent snapshot until they release it.
struct ConfigSnapshot
{
    GlobalConfig                                          globals;
    std::unordered_map<std::string, WeaponConfigOverride> weaponOverrides;
};

static std::mutex                                        s_loadMutex;  // serializes (re)loads
static std::atomic<std::shared_ptr<const ConfigSnapshot>> s_snapshot;

static std::shared_ptr<const ConfigSnapshot> BuildSnapshot();  // fwd decl

// Returns the live snapshot, lazily building it once on first access.
static std::shared_ptr<const ConfigSnapshot> Snapshot()
{
    if (auto s = s_snapshot.load(std::memory_order_acquire)) return s;
    std::lock_guard lock(s_loadMutex);
    auto s = s_snapshot.load(std::memory_order_acquire);
    if (!s) { s = BuildSnapshot(); s_snapshot.store(s, std::memory_order_release); }
    return s;
}

std::shared_ptr<const std::unordered_map<std::string, WeaponConfigOverride>> WeaponOverrides()
{
    auto snap = Snapshot();
    // Aliasing shared_ptr: shares ownership with snap, points at the map member.
    return { snap, &snap->weaponOverrides };
}

// ── Path helpers ──────────────────────────────────────────────────────────────

static std::filesystem::path GetModuleDir()
{
    wchar_t buf[MAX_PATH]{};
    HMODULE hm = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetGlobals), &hm);
    if (hm) GetModuleFileNameW(hm, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

// Returns path of the first existing match, or empty path if neither found.
static std::filesystem::path FindConfigFile(const wchar_t* name)
{
    const auto dir = GetModuleDir();

    // 1. Next to the DLL (distribution).
    auto p = dir / name;
    if (std::filesystem::exists(p)) return p;

    // 2. Two directories up (dev: solution root next to Drgmods.sln).
    std::error_code ec;
    p = std::filesystem::weakly_canonical(dir / L".." / L".." / name, ec);
    if (!ec && std::filesystem::exists(p)) return p;

    return {};
}

// ── Key / Mod parsing ─────────────────────────────────────────────────────────

static std::string ToLower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static Key ParseKey(const std::string& s)
{
    // clang-format off
    static const std::unordered_map<std::string, Key> kTable = {
        // Mouse
        {"mouseleft",    Key::MouseLeft},  {"leftmouse",    Key::MouseLeft},
        {"mouseright",   Key::MouseRight}, {"rightmouse",   Key::MouseRight},
        {"mousemiddle",  Key::MouseMiddle},{"middlemouse",  Key::MouseMiddle},
        {"mousex1",      Key::MouseX1},    {"mousex2",      Key::MouseX2},
        // Letters A–Z
        {"a",Key::A},{"b",Key::B},{"c",Key::C},{"d",Key::D},{"e",Key::E},
        {"f",Key::F},{"g",Key::G},{"h",Key::H},{"i",Key::I},{"j",Key::J},
        {"k",Key::K},{"l",Key::L},{"m",Key::M},{"n",Key::N},{"o",Key::O},
        {"p",Key::P},{"q",Key::Q},{"r",Key::R},{"s",Key::S},{"t",Key::T},
        {"u",Key::U},{"v",Key::V},{"w",Key::W},{"x",Key::X},{"y",Key::Y},
        {"z",Key::Z},
        // Function keys
        {"f1",Key::F1},{"f2",Key::F2}, {"f3",Key::F3}, {"f4",Key::F4},
        {"f5",Key::F5},{"f6",Key::F6}, {"f7",Key::F7}, {"f8",Key::F8},
        {"f9",Key::F9},{"f10",Key::F10},{"f11",Key::F11},{"f12",Key::F12},
        // Navigation / editing
        {"enter",Key::Return},  {"return",Key::Return},
        {"escape",Key::Escape}, {"esc",Key::Escape},
        {"space",Key::Space},   {"backspace",Key::BackSpace},
        {"tab",Key::Tab},       {"delete",Key::Delete},  {"del",Key::Delete},
        {"insert",Key::Insert}, {"ins",Key::Insert},
        {"home",Key::Home},     {"end",Key::End},
        {"pageup",Key::PageUp}, {"pgup",Key::PageUp},
        {"pagedown",Key::PageDown},{"pgdn",Key::PageDown},
        {"left",Key::Left},{"right",Key::Right},{"up",Key::Up},{"down",Key::Down},
        // Punctuation
        {"minus",Key::Minus},       {"equals",Key::Equal},   {"equal",Key::Equal},
        {"comma",Key::Comma},       {"period",Key::Period},  {"dot",Key::Period},
        {"slash",Key::Slash},       {"backslash",Key::Backslash},
        {"semicolon",Key::Semicolon},{"grave",Key::Grave},   {"tilde",Key::Grave},
        {"quote",Key::Quote},
        {"leftbracket",Key::LeftBracket},{"rightbracket",Key::RightBracket},
        // Numpad
        {"num0",Key::Num0},{"num1",Key::Num1},{"num2",Key::Num2},
        {"num3",Key::Num3},{"num4",Key::Num4},{"num5",Key::Num5},
        {"num6",Key::Num6},{"num7",Key::Num7},{"num8",Key::Num8},
        {"num9",Key::Num9},
    };
    // clang-format on

    const auto low = ToLower(s);
    if (auto it = kTable.find(low); it != kTable.end()) return it->second;
    warn("[aim config] unknown key '{}' — defaulting to MouseLeft", s);
    return Key::MouseLeft;
}

// Handles "None", "Ctrl", "Shift", "Alt", and combinations like "Ctrl+Shift".
static Mod ParseMod(const std::string& s)
{
    const auto low = ToLower(s);
    Mod m = Mod::None;
    if (low.find("ctrl")  != std::string::npos) m |= Mod::Ctrl;
    if (low.find("shift") != std::string::npos) m |= Mod::Shift;
    if (low.find("alt")   != std::string::npos) m |= Mod::Alt;
    return m;
}

// ── C++ fallbacks (used when the YAML section is absent) ──────────────────────

static void FallbackIgnoredItemClasses(GlobalConfig& cfg)
{
    for (UClass* cls : {
            APickaxeItem::StaticClass(),
            ADoubleDrillItem::StaticClass(),
            AZipLineItem::StaticClass(),
            AGrapplingHookGun::StaticClass(),
            AFlareGun::StaticClass(),
        })
        if (cls) cfg.IgnoredItemClasses.push_back(cls->Name);
    cfg.IgnoredItemClasses.push_back(FName(L"WPN_PlatformGun_C"));
}

static void FallbackDefaultIgnoreBaseClasses(GlobalConfig& cfg)
{
    cfg.DefaultIgnoreBaseClasses.push_back(FName(L"ENE_Spider_Grunt_Base_C"));
}

// ── Weapon-override parsing (replaces aim_config.json) ───────────────────────

static std::vector<FName> ParseFNameList(const YAML::Node& seq)
{
    std::vector<FName> out;
    if (!seq || !seq.IsSequence()) return out;
    for (const auto& item : seq)
        if (item.IsScalar())
            out.push_back(FName(StringLib::ToWide(item.as<std::string>()).c_str()));
    return out;
}

static void ParseWeapons(const YAML::Node& root,
                         std::unordered_map<std::string, WeaponConfigOverride>& out)
{
    const auto weapons = root["weapons"];
    if (!weapons || !weapons.IsMap()) return;

    for (const auto& pair : weapons)
    {
        const auto cls = pair.first.as<std::string>();
        const auto& w  = pair.second;
        if (!w.IsMap()) continue;

        WeaponConfigOverride& wc = out[cls];
        if (auto v = w["AimbotFOVDeg"];        v) wc.AimbotFOVDeg        = v.as<float>();
        if (auto v = w["SilentAimFOVDeg"];     v) wc.SilentAimFOVDeg     = v.as<float>();
        if (auto v = w["SilentAimRequireLOS"]; v) wc.SilentAimRequireLOS = v.as<bool>();
        if (auto v = w["IgnoreBaseClasses"];   v) wc.IgnoreBaseClasses   = ParseFNameList(v);
        if (auto v = w["ForceIncludeClasses"]; v) wc.ForceIncludeClasses = ParseFNameList(v);
        if (auto v = w["TargetSelector"];      v) wc.TargetSelector      = v.as<std::string>();
    }
}

// ── Core loader ───────────────────────────────────────────────────────────────

// Builds a fresh, immutable config snapshot from disk (or built-in defaults).
// Touches no shared state — the caller swaps it in under s_loadMutex.
static std::shared_ptr<const ConfigSnapshot> BuildSnapshot()
{
    GlobalConfig cfg;        // starts from struct defaults
    std::unordered_map<std::string, WeaponConfigOverride> weaponOverrides;

    auto finalize = [&]() {
        auto snap = std::make_shared<ConfigSnapshot>();
        snap->globals         = std::move(cfg);
        snap->weaponOverrides = std::move(weaponOverrides);
        return snap;
    };

    const auto path = FindConfigFile(L"aimbot.yaml");
    if (path.empty())
    {
        FallbackIgnoredItemClasses(cfg);
        FallbackDefaultIgnoreBaseClasses(cfg);
        info("[aim config] aimbot.yaml not found (checked DLL dir and ../..); using built-in defaults");
        return finalize();
    }

    YAML::Node root;
    try { root = YAML::LoadFile(path.string()); }
    catch (const YAML::Exception& e)
    {
        warn("[aim config] YAML parse error in '{}': {}", path.string(), e.what());
        FallbackIgnoredItemClasses(cfg);
        FallbackDefaultIgnoreBaseClasses(cfg);
        return finalize();
    }

    // The former `globals:` scalars are cvars now (see g_aim* above) — read live,
    // persisted to settings.json. aimbot.yaml keeps only the list/keybind sections.

    // ── wp_sample_offsets ────────────────────────────────────────────────────
    if (const auto n = root["wp_sample_offsets"]; n && n.IsSequence())
    {
        cfg.WpSampleOffsets.clear();
        for (const auto& item : n)
        {
            if (!item.IsSequence() || item.size() < 3) continue;
            cfg.WpSampleOffsets.push_back({
                item[0].as<float>(),
                item[1].as<float>(),
                item[2].as<float>(),
            });
        }
        if (cfg.WpSampleOffsets.empty())
            warn("[aim config] wp_sample_offsets is empty — weakpoint sampling disabled");
    }

    // ── keybinds ─────────────────────────────────────────────────────────────
    if (const auto kb = root["keybinds"]; kb && kb.IsMap())
    {
        auto parseKB = [&](const char* name, Key& outKey, Mod& outMod)
        {
            const auto n = kb[name];
            if (!n || !n.IsMap()) return;
            if (auto k = n["key"]; k) outKey = ParseKey(k.as<std::string>());
            if (auto m = n["mod"]; m) outMod = ParseMod(m.as<std::string>());
        };
        parseKB("aimbot",             cfg.AimbotKey,       cfg.AimbotMod);
        parseKB("recoil_toggle",      cfg.RecoilToggleKey, cfg.RecoilToggleMod);
        parseKB("silent_aim_toggle",  cfg.SilentAimToggleKey, cfg.SilentAimToggleMod);
    }

    // ── ignored_item_classes ─────────────────────────────────────────────────
    // Present in YAML → use exactly those names.
    // Absent → StaticClass() fallback keeps game-update resilience.
    if (const auto n = root["ignored_item_classes"]; n && n.IsSequence())
        cfg.IgnoredItemClasses = ParseFNameList(n);
    else
        FallbackIgnoredItemClasses(cfg);

    // ── default_ignore_base_classes ──────────────────────────────────────────
    if (const auto n = root["default_ignore_base_classes"]; n && n.IsSequence())
        cfg.DefaultIgnoreBaseClasses = ParseFNameList(n);
    else
        FallbackDefaultIgnoreBaseClasses(cfg);

    // ── weapons ──────────────────────────────────────────────────────────────
    ParseWeapons(root, weaponOverrides);

    info("[aim config] loaded from '{}' ({} weapon override(s))",
         path.string(), weaponOverrides.size());
    return finalize();
}

// ── Public API ────────────────────────────────────────────────────────────────

std::shared_ptr<const GlobalConfig> GetGlobals()
{
    auto snap = Snapshot();
    // Aliasing shared_ptr: shares ownership with snap, points at the globals member.
    return { snap, &snap->globals };
}

void ReloadGlobals()
{
    std::lock_guard lock(s_loadMutex);
    s_snapshot.store(BuildSnapshot(), std::memory_order_release);
    // Note: keybinds are registered once at startup via RegisterKeybinds().
    // A full DLL reload is required for keybind changes to take effect.
}

// Triggers the initial lazy load (Resolve* / TargetSelection call this).
void EnsureOverridesLoaded()
{
    (void)Snapshot();
}

// ── Resolve helpers ───────────────────────────────────────────────────────────

float ResolveAimbotFOV(AItem* eq)
{
    auto snap = Snapshot();
    if (eq && eq->Class)
        if (auto it = snap->weaponOverrides.find(eq->Class->Name.ToString());
            it != snap->weaponOverrides.end() && it->second.AimbotFOVDeg)
            return *it->second.AimbotFOVDeg;
    return g_aimFov.get();
}

float ResolveSilentAimFOV(AItem* eq)
{
    auto snap = Snapshot();
    if (eq && eq->Class)
        if (auto it = snap->weaponOverrides.find(eq->Class->Name.ToString());
            it != snap->weaponOverrides.end() && it->second.SilentAimFOVDeg)
            return *it->second.SilentAimFOVDeg;
    return g_silentFov.get();
}

bool ResolveSilentAimRequireLOS(AItem* eq)
{
    auto snap = Snapshot();
    if (eq && eq->Class)
        if (auto it = snap->weaponOverrides.find(eq->Class->Name.ToString());
            it != snap->weaponOverrides.end() && it->second.SilentAimRequireLOS)
            return *it->second.SilentAimRequireLOS;
    return g_silentRequireLos.get();
}

Targeting::SelectorFn ResolveTargetSelector(AItem* eq)
{
    auto snap = Snapshot();
    if (eq && eq->Class)
        if (auto it = snap->weaponOverrides.find(eq->Class->Name.ToString());
            it != snap->weaponOverrides.end() && it->second.TargetSelector)
            return Targeting::Get(*it->second.TargetSelector);
    return Targeting::Get("default");
}

// ── Scalar cvar accessors (no per-weapon layer; live game-thread reads) ─────────
float BodyRadiusScale()     { return g_bodyRadiusScale.get(); }
float BodyRadiusFallback()  { return g_bodyRadiusFallback.get(); }
float GimbalFlipThreshold() { return g_gimbalFlipThreshold.get(); }

} // namespace Config
} // namespace AimAssist
