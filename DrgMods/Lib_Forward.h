#pragma once

#include "SDK/SDK/Basic.hpp"

// FName hash — defined here so every Lib_*.cpp that includes any Lib_*.h
// gets the specialisation before any unordered_map<FName,...> instantiation.
template<>
struct std::hash<SDK::FName>
{
    size_t operator()(const SDK::FName& n) const noexcept
    {
        return std::hash<uint64_t>{}(
            (uint64_t)(uint32_t)n.ComparisonIndex << 32 | n.Number);
    }
};

#include "SDK/SDK/Engine_classes.hpp"
#include "SDK/SDK/FSD_classes.hpp"
#include "SDK/SDK/CoreUObject_Classes.hpp"
#include <spdlog/spdlog.h>

template<typename... Args> inline void trace   (spdlog::format_string_t<Args...> fmt, Args&&... args) { spdlog::trace   (fmt, std::forward<Args>(args)...); }
template<typename... Args> inline void debug   (spdlog::format_string_t<Args...> fmt, Args&&... args) { spdlog::debug   (fmt, std::forward<Args>(args)...); }
template<typename... Args> inline void info    (spdlog::format_string_t<Args...> fmt, Args&&... args) { spdlog::info    (fmt, std::forward<Args>(args)...); }
template<typename... Args> inline void warn    (spdlog::format_string_t<Args...> fmt, Args&&... args) { spdlog::warn    (fmt, std::forward<Args>(args)...); }
template<typename... Args> inline void error   (spdlog::format_string_t<Args...> fmt, Args&&... args) { spdlog::error   (fmt, std::forward<Args>(args)...); }
template<typename... Args> inline void critical(spdlog::format_string_t<Args...> fmt, Args&&... args) { spdlog::critical(fmt, std::forward<Args>(args)...); }

namespace Utils = SDK::InSDKUtils;
namespace Game  = SDK;

using Kismet   = SDK::UKismetSystemLibrary;
using GameLib  = SDK::UGameFunctionLibrary;
using ActorLib = SDK::UActorFunctionLibrary;
using MathLib  = SDK::UKismetMathLibrary;

using uint64    = unsigned long long;
using int64     = signed long long;
using int32     = signed int;
using uint32    = unsigned int;
using int16     = signed short;
using uint16    = unsigned short;
using int8      = signed char;
using uint8     = unsigned char;
using wchar     = wchar_t;


bool         IsValid(const SDK::UObject* Object);
bool         IsValidClass(const SDK::UClass* Class);
SDK::UWorld* GetWorld();
