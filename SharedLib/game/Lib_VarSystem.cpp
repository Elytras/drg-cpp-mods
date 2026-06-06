#ifndef NOMINMAX
#define NOMINMAX               // keep std::min/max usable alongside <Windows.h>
#endif
#define WIN32_LEAN_AND_MEAN
#include "Lib_VarSystem.h"
#include "Saveable.h"
#include "Lib_Utils.h"
#include "Lib_CommandHandler.h"
#include "Lib_GameHooks.h"
#include "StringLib.h"
#include <Windows.h>           // GetModuleHandleEx/GetModuleFileName for settings path
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <variant>

inline const UC::TArray<SDK::APlayerCharacter*>GetAllPlayers()
{
#ifdef RogueCore
    return ActorLib::GetActivePlayerCharacters(GetWorld());
#else
    return ActorLib::GetAllPlayerCharacters(GetWorld());
#endif
}
namespace VarSystem
{
    using namespace SDK;  // re-declare here so .cpp can use SDK types unqualified
    // =========================================================================
    // Global Storage Definitions
    // =========================================================================

    std::unordered_map<std::string, Var>& Vars()
    {
        static std::unordered_map<std::string, Var> s_vars;
        return s_vars;
    }

    std::unordered_map<std::string, BindingFn>& Bindings()
    {
        static std::unordered_map<std::string, BindingFn> s_bindings;
        return s_bindings;
    }

    // =========================================================================
    // Type Name Lookup
    // =========================================================================

    const char* TypeName(VarType t)
    {
        switch (t)
        {
        case VarType::String:  return "string";
        case VarType::Bool:    return "bool";
        case VarType::Int32:   return "int32";
        case VarType::Float:   return "float";
        case VarType::Vector:  return "vector";
        case VarType::Rotator: return "rotator";
        case VarType::Name:    return "fname";
        case VarType::Object:  return "object";
        default:               return "unknown";
        }
    }

    // =========================================================================
    // Var value access (typed store ⇄ string form)
    // =========================================================================

    std::string Var::ToString() const
    {
        switch (type)
        {
        case VarType::Bool:
            { auto* p = std::get_if<bool>(&value); return (p && *p) ? "true" : "false"; }
        case VarType::Int32:
            { auto* p = std::get_if<int32_t>(&value); return std::to_string(p ? *p : 0); }
        case VarType::Float:
            {
                auto* p = std::get_if<float>(&value);
                char buf[32];
                snprintf(buf, sizeof(buf), "%g", p ? *p : 0.f);
                return buf;
            }
        case VarType::Object:
            { UObject* o = AsObject(); return (o && Kismet::IsValid(o)) ? o->GetName() : ""; }
        default:   // String / Vector / Rotator / Name — stored verbatim
            { auto* p = std::get_if<std::string>(&value); return p ? *p : std::string{}; }
        }
    }

    bool Var::AsBool() const
    {
        if (auto* p = std::get_if<bool>(&value)) return *p;
        const std::string s = ToString();
        return s == "true" || s == "1" || s == "True";
    }

    int32_t Var::AsInt() const
    {
        if (auto* p = std::get_if<int32_t>(&value)) return *p;
        return (int32_t)SafeStoll(ToString());
    }

    float Var::AsFloat() const
    {
        if (auto* p = std::get_if<float>(&value)) return *p;
        return SafeStof(ToString());
    }

    UObject* Var::AsObject() const
    {
        auto* p = std::get_if<UObject*>(&value);
        return p ? *p : nullptr;
    }

    // =========================================================================
    // Core Operations
    // =========================================================================

    void Clear()
    {
        // Keep Persistent (config) vars across a reload/`retry`; wipe only session
        // scratch vars. Keeping the live entries also preserves their declared types
        // for the Saveable handles (whose statics don't re-run on an in-process reload).
        auto& vars = Vars();
        for (auto it = vars.begin(); it != vars.end(); )
        {
            if (it->second.flags & VarFlags::Persistent) ++it;
            else it = vars.erase(it);
        }
    }

    void RegisterBinding(const std::string& name, BindingFn fn)
    {
        Bindings()[name] = std::move(fn);
    }

    // =========================================================================
    // Helper Converters
    // =========================================================================

    ExpandResult BindObject(UObject* obj)
    {
        if (!obj || !Kismet::IsValid(obj)) return { "", nullptr, false };
        return { "", obj, true };
    }

    ExpandResult BindToken(const std::string& token)
    {
        return { token, nullptr, true };
    }

    // =========================================================================
    // Registration and Parsing
    // =========================================================================

    void RegisterBuiltinBindings()
    {
        RegisterBinding("localplayer", []() {
            return BindObject(reinterpret_cast<UObject*>(GetLocalPlayer()));
            });

        RegisterBinding("world", []() {
            return BindObject(reinterpret_cast<UObject*>(GetWorld()));
            });

        RegisterBinding("randomplayer", []() {
            auto Players = GetAllPlayers();
            std::vector<APlayerCharacter*> validPlayers;
            for (auto p : Players)
                if (p && p != GetLocalPlayer())
                    validPlayers.push_back(p);

            if (validPlayers.empty())
                return BindObject(nullptr);

            static thread_local std::mt19937 rng{ std::random_device{}() };
            std::uniform_int_distribution<size_t> dist(0, validPlayers.size() - 1);
            APlayerCharacter* randomPlayer = validPlayers[dist(rng)];
            return BindObject(reinterpret_cast<UObject*>(randomPlayer));
            });
    }

    Var Parse(const std::string& raw)
    {
        // Check for vec: prefix — validate "x,y,z", store the inner text verbatim
        // (vectors are consumed as strings; commands reparse them).
        if (raw.size() > 4 && raw.substr(0, 4) == "vec:")
        {
            std::string inner = raw.substr(4);
            float x, y, z;
            if (sscanf_s(inner.c_str(), "%f,%f,%f", &x, &y, &z) == 3)
                return { VarType::Vector, inner };
            return { VarType::String, raw };
        }

        // Check for rot: prefix
        if (raw.size() > 4 && raw.substr(0, 4) == "rot:")
        {
            std::string inner = raw.substr(4);
            float p, y, r;
            if (sscanf_s(inner.c_str(), "%f,%f,%f", &p, &y, &r) == 3)
                return { VarType::Rotator, inner };
            return { VarType::String, raw };
        }

        // Check for name: prefix
        if (raw.size() > 5 && raw.substr(0, 5) == "name:")
            return { VarType::Name, raw.substr(5) };

        // Check for boolean
        if (raw == "true")  return { VarType::Bool, true };
        if (raw == "false") return { VarType::Bool, false };

        // Check for int32
        {
            bool isInt = !raw.empty();
            size_t start = (raw[0] == '-') ? 1 : 0;
            if (start == raw.size())
                isInt = false;
            for (size_t i = start; i < raw.size() && isInt; ++i)
                if (!isdigit((unsigned char)raw[i]))
                    isInt = false;
            if (isInt)
                return { VarType::Int32, (int32_t)SafeStoll(raw) };
        }

        // Check for float
        if (raw.find('.') != std::string::npos && raw.find(' ') == std::string::npos)
        {
            try
            {
                size_t pos;
                float f = std::stof(raw, &pos);
                if (pos == raw.size())
                    return { VarType::Float, f };
            }
            catch (...)
            {
            }
        }

        return { VarType::String, raw };
    }

    ExpandResult Expand(const std::string& token)
    {
        // Handle function binding: fn:<name>
        if (token.size() > 3 && token.substr(0, 3) == "fn:")
        {
            std::string name = token.substr(3);
            auto it = Bindings().find(name);
            if (it == Bindings().end())
            {
                warn("[var] fn:'{}' not registered", name);
                return { "", nullptr, false };
            }
            ExpandResult r = it->second();
            if (!r.isValid)
            {
                warn("[var] fn:'{}' returned invalid", name);
                return { "", nullptr, false };
            }
            if (r.object)
                info("[var] fn:'{}' → [object] '{}'", name, r.object->GetName());
            else
                info("[var] fn:'{}' → '{}'", name, r.token);
            return r;
        }

        // Handle variable: var:<name>
        if (token.size() > 4 && token.substr(0, 4) == "var:")
        {
            std::string name = token.substr(4);
            auto it = Vars().find(name);
            if (it == Vars().end())
            {
                warn("[var] '{}' not defined", name);
                return { "", nullptr, false };
            }
            const Var& v = it->second;
            if (v.type == VarType::Object)
            {
                UObject* o = v.AsObject();
                if (!o || !Kismet::IsValid(o))
                {
                    warn("[var] '{}' points to destroyed object — clearing", name);
                    Vars().erase(it);
                    return { "", nullptr, false };
                }
                info("[var] '{}' → object '{}'", name, o->GetName());
                return { "", o, true };
            }
            return { v.ToString(), nullptr, true };
        }

        return { token, nullptr, true };
    }

    void Print(const std::string& name, const Var& v)
    {
        switch (v.type)
        {
        case VarType::Vector:
            info("[var] {} = vec:{} (vector)", name, v.ToString());
            break;
        case VarType::Rotator:
            info("[var] {} = rot:{} (rotator)", name, v.ToString());
            break;
        case VarType::Name:
            info("[var] {} = name:{} (fname)", name, v.ToString());
            break;
        case VarType::Object:
        {
            UObject* o = v.AsObject();
            if (o && Kismet::IsValid(o))
                info("[var] {} = [object] {} ({})", name, o->GetName(),
                    o->Class ? o->Class->GetName() : "?");
            else
                info("[var] {} = [object] <stale/null>", name);
            break;
        }
        default:
            info("[var] {} = {} ({})", name, v.ToString(), TypeName(v.type));
            break;
        }
    }

    void Cmd_Set(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 3)
        {
            warn("[var] Usage: set <n> <value>");
            return;
        }
        const std::string& name = ctx.Arg(1);
        std::string raw = ctx.Arg(2);
        for (size_t i = 3; i < ctx.ArgCount(); ++i)
            raw += ' ' + ctx.Arg(i);

        Var v = Parse(raw);
        auto& vars = Vars();
        if (auto it = vars.find(name); it != vars.end())
            v.flags = it->second.flags;   // preserve persistence/visibility across a re-set
        vars[name] = v;
        if (v.flags & VarFlags::Persistent) MarkSettingsDirty();
        Print(name, v);
    }

    void Cmd_Get(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 2)
        {
            warn("[var] Usage: get <n>");
            return;
        }
        const std::string& name = ctx.Arg(1);
        auto it = Vars().find(name);
        if (it == Vars().end())
        {
            warn("[var] '{}' not defined", name);
            return;
        }
        Print(name, it->second);
    }

    void Cmd_Unset(const CommandContext& ctx)
    {
        if (ctx.ArgCount() < 2)
        {
            warn("[var] Usage: unset <n>");
            return;
        }
        const std::string& name = ctx.Arg(1);
        if (Vars().erase(name))
            info("[var] '{}' unset", name);
        else
            warn("[var] '{}' was not defined", name);
    }

    void Cmd_Vars(const CommandContext&)
    {
        if (Vars().empty() && Bindings().empty())
        {
            info("[var] No variables defined.");
            return;
        }
        if (!Vars().empty())
        {
            info("[var] {} variable(s):", Vars().size());
            for (const auto& [n, v] : Vars())
                Print(n, v);
        }
        if (!Bindings().empty())
        {
            info("[var] {} binding(s) (fn:<n>):", Bindings().size());
            for (const auto& [n, fn] : Bindings())
            {
                ExpandResult r = fn();
                if (!r.isValid)
                    info("[var]   fn:{} = <invalid>", n);
                else if (r.object)
                    info("[var]   fn:{} = [object] {} ({})", n,
                        r.object->GetName(), r.object->Class ? r.object->Class->GetName() : "?");
                else
                    info("[var]   fn:{} = {}", n, r.token);
            }
        }
    }

    // =========================================================================
    // Settings persistence (Persistent vars ⇄ settings.json)
    // =========================================================================

    namespace
    {
        bool s_settingsDirty = false;

        // settings.json next to the containing DLL (resolved via this function's own
        // address, so DrgMods.dll and RcMods.dll each get their own file).
        std::string SettingsPath()
        {
            wchar_t buf[MAX_PATH]{};
            HMODULE hm = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&SettingsPath), &hm);
            if (!hm) return {};
            GetModuleFileNameW(hm, buf, MAX_PATH);
            std::filesystem::path p(buf);
            if (p.empty()) return {};
            return (p.parent_path() / L"settings.json").string();
        }
    }

    void MarkSettingsDirty() { s_settingsDirty = true; }

    void SaveSettings()
    {
        const std::string path = SettingsPath();
        if (path.empty()) { warn("[settings] cannot resolve settings.json path"); return; }

        nlohmann::json j = nlohmann::json::object();
        for (const auto& [name, v] : Vars())
        {
            if (!(v.flags & VarFlags::Persistent)) continue;
            switch (v.type)
            {
            case VarType::Bool:   j[name] = v.AsBool();  break;
            case VarType::Int32:  j[name] = v.AsInt();   break;
            case VarType::Float:  j[name] = v.AsFloat(); break;
            case VarType::Object: break;                 // runtime pointer — not serializable
            default:              j[name] = v.ToString(); break;  // String/Vector/Rotator/Name
            }
        }

        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) { warn("[settings] save FAILED: {}", path); return; }
        f << j.dump(2);
        s_settingsDirty = false;
        info("[settings] saved {} ({} entr{})", path, j.size(), j.size() == 1 ? "y" : "ies");
    }

    void FlushSettings(bool force)
    {
        if (force || s_settingsDirty) SaveSettings();
    }

    void LoadSettings()
    {
        const std::string path = SettingsPath();
        if (path.empty()) return;
        std::ifstream f(path, std::ios::binary);
        if (!f) return;   // no settings file yet — first run

        nlohmann::json j;
        try { f >> j; }
        catch (const nlohmann::json::exception& e)
        {
            warn("[settings] parse error in '{}': {}", path, e.what());
            return;
        }
        if (!j.is_object()) return;

        // Apply each value in its JSON-natural type, flagged Persistent. The typed
        // Saveable<T> accessors coerce on read, and a Saveable's ctor re-normalises the
        // entry to its declared T — so load order vs. Saveable static-init doesn't matter.
        auto& vars = Vars();
        size_t n = 0;
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            const auto& val = it.value();
            Var v;
            if      (val.is_boolean())         { v.type = VarType::Bool;   v.value = val.get<bool>(); }
            else if (val.is_number_integer())  { v.type = VarType::Int32;  v.value = (int32_t)val.get<long long>(); }
            else if (val.is_number())          { v.type = VarType::Float;  v.value = val.get<float>(); }
            else if (val.is_string())          { v.type = VarType::String; v.value = val.get<std::string>(); }
            else continue;
            v.flags = VarFlags::Persistent;
            if (auto vit = vars.find(it.key()); vit != vars.end())
                v.flags |= vit->second.flags;   // keep any flags a Saveable already set
            vars[it.key()] = std::move(v);
            ++n;
        }
        info("[settings] loaded {} ({} entr{})", path, n, n == 1 ? "y" : "ies");
    }

} // namespace VarSystem

// Validation: force-instantiate the supported Saveable<T> specialisations so their
// members are fully type-checked even before a real client exists. Remove once a
// feature owns Saveable members.
namespace VarSystem
{
    template class Saveable<bool>;
    template class Saveable<int32_t>;
    template class Saveable<float>;
    template class Saveable<std::string>;
}
