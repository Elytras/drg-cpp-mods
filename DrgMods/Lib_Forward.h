#pragma once

#include "SDK/SDK/Basic.hpp"
#include "SDK/SDK/Engine_classes.hpp"
#include "SDK/SDK/FSD_classes.hpp"
#include "SDK/SDK/CoreUObject_Classes.hpp"
#include <spdlog/spdlog.h>

using namespace SDK;
using namespace spdlog;
using Kismet   = UKismetSystemLibrary;
using GameLib  = UGameFunctionLibrary;
using ActorLib = UActorFunctionLibrary;
using MathLib  = UKismetMathLibrary;
using uint64 = unsigned long long;
using int64  = long long;
using int32  = int;
using uint32 = unsigned int;
namespace Utils = InSDKUtils;
namespace l = spdlog;

inline bool    IsValid(const UObject* Object);
inline bool    IsValidClass(const UClass* Class);
inline UWorld* GetWorld();
