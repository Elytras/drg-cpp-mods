#pragma once

#include "SDK/SDK/Basic.hpp"
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


inline bool    IsValid(const UObject* Object);
inline bool    IsValidClass(const UClass* Class);
inline UWorld* GetWorld();
