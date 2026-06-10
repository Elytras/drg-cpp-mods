#pragma once
// Lib_Scan.h — Generic, SDK-agnostic scan helpers (replicated/server RPCs).
//
// Each .cpp consumer's AdditionalIncludeDirectories supplies the right per-game
// SDK headers, so this same source compiles correctly into both DrgMods.dll
// and RcMods.dll.

#include <string>
#include <vector>
#include "Lib_Forward.h"

namespace Scan
{
    // "<OwnerClass>::<OwnerName>::<FuncName>" — unique identifier used to
    // disambiguate replicated UFunctions across multiple instances/components.
    std::string BuildExplicitCallName(SDK::UObject* Owner, SDK::UFunction* Func);

    // Pretty-printed function signature: "Name(Arg0: Type, Arg1: Type)".
    // Wraps to multi-line with box-drawing characters when too wide.
    std::string BuildFuncSig(SDK::UFunction* Func);

    // Append unique, sorted signatures of every Net+NetServer UFunction visible
    // on `Obj` (walks the full class hierarchy). Pure data — no logging.
    void ScanFunctions(SDK::UObject* Obj, std::vector<std::string>& out);

    // Iterate every UClass in GObjects and log its Net+NetServer UFunctions.
    // Returns {classCount, funcCount}. Logs incrementally via spdlog.
    struct ScanAllResult { int classes = 0; int functions = 0; };
    ScanAllResult ScanAllClasses();
}
