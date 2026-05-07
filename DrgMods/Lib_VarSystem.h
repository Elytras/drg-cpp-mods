#pragma once
// Lib_VarSystem.h — Session-scoped variable storage + function bindings.

#include <functional>
#include <string>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include "Lib_Forward.h"
#include "Lib_Utils.h"

// CommandContext forward declaration (defined in Lib_CommandHandler.h which comes after)
struct CommandContext;

namespace VarSystem
{
    enum class VarType { String, Bool, Int32, Float, Vector, Rotator, Name, Object };

    static const char* TypeName(VarType t)
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

    struct Var
    {
        VarType     type = VarType::String;
        std::string token;
        UObject* object = nullptr;
    };

    struct ExpandResult
    {
        std::string token;
        UObject* object = nullptr;
        bool        isValid = true;
    };

    inline std::unordered_map<std::string, Var>                        g_Vars;
    using BindingFn = std::function<ExpandResult()>;
    inline std::unordered_map<std::string, BindingFn>                  g_Bindings;

    inline void Clear() { g_Vars.clear(); }

    inline void RegisterBinding(const std::string& name, BindingFn fn) { g_Bindings[name] = std::move(fn); }

    inline ExpandResult BindObject(UObject* obj)
    {
        if (!obj || !Kismet::IsValid(obj)) return { "", nullptr, false };
        return { "", obj, true };
    }
    inline ExpandResult BindToken(const std::string& token) { return { token, nullptr, true }; }

    inline void RegisterBuiltinBindings()
    {
        RegisterBinding("localplayer", []() { return BindObject(reinterpret_cast<UObject*>(GetLocalPlayer())); });
        RegisterBinding("world", []() { return BindObject(reinterpret_cast<UObject*>(GetWorld())); });
        RegisterBinding("randomplayer", []() {
            auto Players = ActorLib::GetAllPlayerCharacters(nullptr);
            std::vector<APlayerCharacter*> validPlayers;
            for (auto p : Players) if (p && p != GetLocalPlayer()) validPlayers.push_back(p);
            if (validPlayers.empty()) return BindObject(nullptr);
            srand(static_cast<uint32>(time(nullptr)));
            APlayerCharacter* randomPlayer = validPlayers[rand() % validPlayers.size()];
            return BindObject(reinterpret_cast<UObject*>(randomPlayer));
            });
    }

    inline Var Parse(const std::string& raw)
    {
        if (raw.size() > 4 && raw.substr(0, 4) == "vec:")
        {
            std::string inner = raw.substr(4); float x, y, z;
            if (sscanf_s(inner.c_str(), "%f,%f,%f", &x, &y, &z) == 3) return { VarType::Vector, inner };
            return { VarType::String, raw };
        }
        if (raw.size() > 4 && raw.substr(0, 4) == "rot:")
        {
            std::string inner = raw.substr(4); float p, y, r;
            if (sscanf_s(inner.c_str(), "%f,%f,%f", &p, &y, &r) == 3) return { VarType::Rotator, inner };
            return { VarType::String, raw };
        }
        if (raw.size() > 5 && raw.substr(0, 5) == "name:") return { VarType::Name, raw.substr(5) };
        if (raw == "true" || raw == "false") return { VarType::Bool, raw };
        {
            bool isInt = !raw.empty();
            size_t start = (raw[0] == '-') ? 1 : 0;
            if (start == raw.size()) isInt = false;
            for (size_t i = start; i < raw.size() && isInt; ++i)
                if (!isdigit((unsigned char)raw[i])) isInt = false;
            if (isInt) return { VarType::Int32, raw };
        }
        if (raw.find('.') != std::string::npos && raw.find(' ') == std::string::npos)
        {
            try { size_t pos; (std::stof(raw, &pos)); if (pos == raw.size()) return { VarType::Float, raw }; }
            catch (...) {}
        }
        return { VarType::String, raw };
    }

    inline ExpandResult Expand(const std::string& token)
    {
        if (token.size() > 3 && token.substr(0, 3) == "fn:")
        {
            std::string name = token.substr(3);
            auto it = g_Bindings.find(name);
            if (it == g_Bindings.end()) { spdlog::warn("[var] fn:'{}' not registered", name); return { "", nullptr, false }; }
            ExpandResult r = it->second();
            if (!r.isValid) { spdlog::warn("[var] fn:'{}' returned invalid", name); return { "", nullptr, false }; }
            if (r.object) spdlog::info("[var] fn:'{}' → [object] '{}'", name, r.object->GetName());
            else          spdlog::info("[var] fn:'{}' → '{}'", name, r.token);
            return r;
        }
        if (token.size() > 4 && token.substr(0, 4) == "var:")
        {
            std::string name = token.substr(4);
            auto it = g_Vars.find(name);
            if (it == g_Vars.end()) { spdlog::warn("[var] '{}' not defined", name); return { "", nullptr, false }; }
            const Var& v = it->second;
            if (v.type == VarType::Object)
            {
                if (!v.object || !Kismet::IsValid(v.object))
                {
                    spdlog::warn("[var] '{}' points to destroyed object — clearing", name); g_Vars.erase(it); return { "", nullptr, false };
                }
                spdlog::info("[var] '{}' → object '{}'", name, v.object->GetName());
                return { "", v.object, true };
            }
            return { v.token, nullptr, true };
        }
        return { token, nullptr, true };
    }

    inline void Print(const std::string& name, const Var& v)
    {
        switch (v.type)
        {
        case VarType::Vector:  spdlog::info("[var] {} = vec:{} (vector)", name, v.token); break;
        case VarType::Rotator: spdlog::info("[var] {} = rot:{} (rotator)", name, v.token); break;
        case VarType::Name:    spdlog::info("[var] {} = name:{} (fname)", name, v.token); break;
        case VarType::Object:
            if (v.object && Kismet::IsValid(v.object))
                spdlog::info("[var] {} = [object] {} ({})", name, v.object->GetName(),
                    v.object->Class ? v.object->Class->GetName() : "?");
            else spdlog::info("[var] {} = [object] <stale/null>", name);
            break;
        default: spdlog::info("[var] {} = {} ({})", name, v.token, TypeName(v.type)); break;
        }
    }

    inline void Cmd_Set(const CommandContext& ctx);
    inline void Cmd_Get(const CommandContext& ctx);
    inline void Cmd_Unset(const CommandContext& ctx);
    inline void Cmd_Vars(const CommandContext& ctx);

} // namespace VarSystem