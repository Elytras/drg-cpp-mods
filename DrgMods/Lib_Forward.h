#pragma once
// Lib_Forward.h — Namespace aliases and forward declarations.

#include "SDK/SDK/Basic.hpp"
#include "SDK/SDK/Engine_classes.hpp"
#include "SDK/SDK/FSD_classes.hpp"
#include <fmt/core.h>
#include <spdlog/spdlog.h>
using namespace SDK;
using Kismet = UKismetSystemLibrary;
using GameLib = UGameFunctionLibrary;
using ActorLib = UActorFunctionLibrary;
using MathLib = UKismetMathLibrary;
namespace Utils = InSDKUtils;
namespace BFIU = BasicFilesImpleUtils;
namespace l = spdlog;

// Forward declarations
inline bool    IsValid(const UObject* Object);
inline bool    IsValidClass(const UClass* Class);
inline UWorld* GetWorld();