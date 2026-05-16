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

using namespace SDK;
using namespace spdlog;
namespace Utils = InSDKUtils;
namespace l = spdlog;

using Kismet   = UKismetSystemLibrary;
using GameLib  = UGameFunctionLibrary;
using ActorLib = UActorFunctionLibrary;
using MathLib  = UKismetMathLibrary;

using uint64    = unsigned long long;
using int64     = signed long long;
using int32     = signed int;
using uint32    = unsigned int;
using int16     = signed short;
using uint16    = unsigned short;
using int8      = signed char;
using uint8     = unsigned char;
using wchar     = wchar_t;


bool    IsValid(const UObject* Object);
bool    IsValidClass(const UClass* Class);
UWorld* GetWorld();
