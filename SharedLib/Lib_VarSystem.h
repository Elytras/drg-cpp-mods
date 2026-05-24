#pragma once
// Lib_VarSystem.h — Session-scoped variable storage + function bindings.

#include <functional>
#include <string>
#include <unordered_map>
#include "Lib_Forward.h"

struct CommandContext;

namespace VarSystem
{
    enum class VarType { String, Bool, Int32, Float, Vector, Rotator, Name, Object };

    // Type name lookup
    const char* TypeName(VarType t);

    struct Var
    {
        VarType          type = VarType::String;
        std::string      token;
        SDK::UObject*    object = nullptr;
    };

    struct ExpandResult
    {
        std::string      token;
        SDK::UObject*    object = nullptr;
        bool             isValid = true;
    };

    extern std::unordered_map<std::string, Var> g_Vars;

    using BindingFn = std::function<ExpandResult()>;
    extern std::unordered_map<std::string, BindingFn> g_Bindings;

    // Core operations
    void Clear();
    void RegisterBinding(const std::string& name, BindingFn fn);

    // Helper converters
    ExpandResult BindObject(SDK::UObject* obj);
    ExpandResult BindToken(const std::string& token);

    // Registration and parsing
    void RegisterBuiltinBindings();
    Var Parse(const std::string& raw);
    ExpandResult Expand(const std::string& token);
    void Print(const std::string& name, const Var& v);

    void Cmd_Set    (const CommandContext& ctx);
    void Cmd_Get    (const CommandContext& ctx);
    void Cmd_Unset  (const CommandContext& ctx);
    void Cmd_Vars   (const CommandContext& ctx);

} // namespace VarSystem
