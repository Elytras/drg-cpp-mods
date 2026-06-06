#pragma once
// Lib_VarSystem.h — Session-scoped variable storage + function bindings.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include "Lib_Forward.h"

struct CommandContext;

namespace VarSystem
{
    enum class VarType { String, Bool, Int32, Float, Vector, Rotator, Name, Object };

    // Type name lookup
    const char* TypeName(VarType t);

    // Persistence/visibility attributes — orthogonal to type. The cvar foundation:
    // a `Saveable<T>` (next step) sets Persistent so the entry round-trips to the
    // settings file. Nothing consumes these yet; they exist so the store is ready.
    namespace VarFlags { enum : uint32_t { None = 0, Persistent = 1u << 0, ReadOnly = 1u << 1, Hidden = 1u << 2 }; }

    // Canonical typed value. Scalars (Bool/Int32/Float) and Object are stored typed;
    // String/Vector/Rotator/Name share the std::string alternative — they're consumed
    // as strings everywhere (commands reparse them via Expand), and storing them typed
    // would drag UE4-vs-UE5 FVector divergence + eager FName interning into the store
    // for no reader. The `type` tag distinguishes the string-family members.
    using Value = std::variant<std::string, bool, int32_t, float, SDK::UObject*>;

    struct Var
    {
        VarType   type  = VarType::String;
        Value     value = std::string{};
        uint32_t  flags = 0;

        VarType Type() const { return type; }

        // Display / serialize form. Scalars are formatted (Float via %g, so 90.0 → "90");
        // String/Vector/Rotator/Name return their stored text verbatim; Object → name.
        std::string ToString() const;

        // Typed reads. If the stored alternative differs from T, coerce from ToString()
        // so a Saveable<T> can sit over a differently-typed entry.
        bool          AsBool()   const;
        int32_t       AsInt()    const;
        float         AsFloat()  const;
        std::string   AsString() const { return ToString(); }
        SDK::UObject* AsObject() const;
    };

    struct ExpandResult
    {
        std::string      token;
        SDK::UObject*    object = nullptr;
        bool             isValid = true;
    };

    // The variable store + the binding table. Function-local statics (Meyers
    // singletons) so they're constructed on first use — a Saveable<T> declared as a
    // file-scope static in any TU can register into Vars() during static init without
    // depending on (unspecified) cross-TU init order.
    std::unordered_map<std::string, Var>& Vars();

    using BindingFn = std::function<ExpandResult()>;
    std::unordered_map<std::string, BindingFn>& Bindings();

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
