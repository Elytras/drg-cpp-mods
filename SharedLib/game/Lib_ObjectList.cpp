// Lib_ObjectList.cpp — game-thread UObject enumeration for the overlay Objects tab.
// Compiled once per game DLL against that game's SDK (same model as Lib_ActorList.cpp).

#include "Lib_ObjectList.h"
#include "Lib_ObjectCast.h"   // IsValidRaw, IsBlueprintClass
#include "Lib_Forward.h"       // SDK types + APawn

using namespace SDK;

namespace ObjectList
{
    static bool IsInternalClass(const std::string& name)
    {
        static constexpr const char* const kNames[] = {
            "Class", "Function", "Enum", "ScriptStruct", "Package",
            "BlueprintGeneratedClass", "WidgetBlueprintGeneratedClass",
            "AnimBlueprintGeneratedClass", "UserDefinedEnum",
            "DelegateFunction", "SparseDelegateFunction"
        };
        for (auto* n : kNames)
            if (name == n) return true;
        return false;
    }

    std::vector<Row> Snapshot(size_t maxRows)
    {
        std::vector<Row> out;
        if (!UObject::GObjects) return out;

        UClass* pawnClass = APawn::StaticClass();
        out.reserve(std::min(maxRows, (size_t)2048));
        const int total = UObject::GObjects->Num();
        for (int i = 0; i < total && out.size() < maxRows; ++i)
        {
            UObject* obj = UObject::GObjects->GetByIndex(i);
            if (!obj || !IsValidRaw(obj)) continue;

            Row r;
            r.addr       = (uint64_t)obj;
            r.outerAddr  = (uint64_t)obj->Outer;
            r.classAddr  = (uint64_t)obj->Class;
            r.className  = obj->Class ? obj->Class->GetName() : "?";
            r.name       = obj->GetName();
            r.outer      = obj->Outer ? obj->Outer->GetName() : "";
            r.isPawn     = (pawnClass != nullptr && obj->IsA(pawnClass));
            r.isInternal = IsInternalClass(r.className);
            r.isDefault  = obj->IsDefaultObject();
            r.isBP       = IsBlueprintClass(obj->Class);
            out.push_back(std::move(r));
        }
        return out;
    }
}
