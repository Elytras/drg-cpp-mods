#include "Lib_VarSystem.h"
#include "Lib_Utils.h"
#include "Lib_CommandHandler.h"
#include "Lib_GameHooks.h"
#include "StringLib.h"
#include <cctype>
#include <cstdlib>
#include <ctime>

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

    std::unordered_map<std::string, Var> g_Vars;
    std::unordered_map<std::string, BindingFn> g_Bindings;

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
    // Core Operations
    // =========================================================================

    void Clear()
    {
        g_Vars.clear();
    }

    void RegisterBinding(const std::string& name, BindingFn fn)
    {
        g_Bindings[name] = std::move(fn);
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

            srand(static_cast<uint32>(time(nullptr)));
            APlayerCharacter* randomPlayer = validPlayers[rand() % validPlayers.size()];
            return BindObject(reinterpret_cast<UObject*>(randomPlayer));
            });
    }

    Var Parse(const std::string& raw)
    {
        // Check for vec: prefix
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
        if (raw == "true" || raw == "false")
            return { VarType::Bool, raw };

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
                return { VarType::Int32, raw };
        }

        // Check for float
        if (raw.find('.') != std::string::npos && raw.find(' ') == std::string::npos)
        {
            try
            {
                size_t pos;
                (void)std::stof(raw, &pos);
                if (pos == raw.size())
                    return { VarType::Float, raw };
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
            auto it = g_Bindings.find(name);
            if (it == g_Bindings.end())
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
            auto it = g_Vars.find(name);
            if (it == g_Vars.end())
            {
                warn("[var] '{}' not defined", name);
                return { "", nullptr, false };
            }
            const Var& v = it->second;
            if (v.type == VarType::Object)
            {
                if (!v.object || !Kismet::IsValid(v.object))
                {
                    warn("[var] '{}' points to destroyed object — clearing", name);
                    g_Vars.erase(it);
                    return { "", nullptr, false };
                }
                info("[var] '{}' → object '{}'", name, v.object->GetName());
                return { "", v.object, true };
            }
            return { v.token, nullptr, true };
        }

        return { token, nullptr, true };
    }

    void Print(const std::string& name, const Var& v)
    {
        switch (v.type)
        {
        case VarType::Vector:
            info("[var] {} = vec:{} (vector)", name, v.token);
            break;
        case VarType::Rotator:
            info("[var] {} = rot:{} (rotator)", name, v.token);
            break;
        case VarType::Name:
            info("[var] {} = name:{} (fname)", name, v.token);
            break;
        case VarType::Object:
            if (v.object && Kismet::IsValid(v.object))
                info("[var] {} = [object] {} ({})", name, v.object->GetName(),
                    v.object->Class ? v.object->Class->GetName() : "?");
            else
                info("[var] {} = [object] <stale/null>", name);
            break;
        default:
            info("[var] {} = {} ({})", name, v.token, TypeName(v.type));
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
        g_Vars[name] = v;
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
        auto it = g_Vars.find(name);
        if (it == g_Vars.end())
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
        if (g_Vars.erase(name))
            info("[var] '{}' unset", name);
        else
            warn("[var] '{}' was not defined", name);
    }

    void Cmd_Vars(const CommandContext&)
    {
        if (g_Vars.empty() && g_Bindings.empty())
        {
            info("[var] No variables defined.");
            return;
        }
        if (!g_Vars.empty())
        {
            info("[var] {} variable(s):", g_Vars.size());
            for (const auto& [n, v] : g_Vars)
                Print(n, v);
        }
        if (!g_Bindings.empty())
        {
            info("[var] {} binding(s) (fn:<n>):", g_Bindings.size());
            for (const auto& [n, fn] : g_Bindings)
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

} // namespace VarSystem
