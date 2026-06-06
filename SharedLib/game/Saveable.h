#pragma once
// Saveable.h — typed, persistable lens over a VarSystem entry (the cvar handle).
//
// A Saveable<T> owns no value of its own: on construction it declares/binds an entry
// in VarSystem::Vars() (default + flags) and thereafter reads/writes it typed. With
// the Persistent flag (the default) the entry round-trips through settings.json via
// VarSystem's Load/FlushSettings. Construct one as a file-scope static or as a member
// that outlives its use — it registers on construction and never unregisters.
//
// Layer: game. Supported T: bool, int32_t, float, std::string. (Object/Vector/Rotator/
// Name vars exist in VarSystem but aren't meaningful as typed persistent settings.)

#include "Lib_VarSystem.h"

#include <string>
#include <type_traits>
#include <utility>

namespace VarSystem
{
    template<class T> constexpr VarType TypeOf();
    template<> constexpr VarType TypeOf<bool>()        { return VarType::Bool; }
    template<> constexpr VarType TypeOf<int32_t>()     { return VarType::Int32; }
    template<> constexpr VarType TypeOf<float>()       { return VarType::Float; }
    template<> constexpr VarType TypeOf<std::string>() { return VarType::String; }

    template<class T>
    class Saveable
    {
        static_assert(std::is_same_v<T, bool> || std::is_same_v<T, int32_t> ||
                      std::is_same_v<T, float> || std::is_same_v<T, std::string>,
                      "Saveable<T>: T must be bool, int32_t, float, or std::string");
    public:
        Saveable(std::string name, T def, uint32_t flags = VarFlags::Persistent)
            : name_(std::move(name)), def_(std::move(def)), flags_(flags)
        {
            auto& vars = Vars();
            auto it = vars.find(name_);
            if (it == vars.end())
            {
                Var v;
                v.type  = TypeOf<T>();
                v.value = def_;
                v.flags = flags_;
                vars.emplace(name_, std::move(v));
            }
            else
            {
                // Already present (e.g. loaded from disk before this static ran):
                // keep the value but normalise it to T and ensure our flags.
                T cur = Read(it->second);
                it->second.type   = TypeOf<T>();
                it->second.value  = cur;
                it->second.flags |= flags_;
            }
        }

        T get() const
        {
            auto& vars = Vars();
            auto it = vars.find(name_);
            return it == vars.end() ? def_ : Read(it->second);
        }

        void set(const T& v)
        {
            Var& e = Vars()[name_];
            e.type   = TypeOf<T>();
            e.value  = v;
            e.flags |= flags_;
            if (flags_ & VarFlags::Persistent) MarkSettingsDirty();
        }

        operator T() const { return get(); }
        Saveable& operator=(const T& v) { set(v); return *this; }
        const std::string& name() const { return name_; }

    private:
        static T Read(const Var& e)
        {
            if constexpr (std::is_same_v<T, bool>)         return e.AsBool();
            else if constexpr (std::is_same_v<T, int32_t>) return e.AsInt();
            else if constexpr (std::is_same_v<T, float>)   return e.AsFloat();
            else                                           return e.AsString();
        }

        std::string name_;
        T           def_;
        uint32_t    flags_;
    };
}
