#pragma once
// Lib_ObjectList.h — SDK-free snapshot of all live UObjects for the overlay Objects tab.
// Snapshot() walks GObjects; must be called on the game thread.

#include <cstdint>
#include <string>
#include <vector>

namespace ObjectList
{
    struct Row
    {
        std::string className;
        std::string name;
        std::string outer;
        uint64_t    addr       = 0;
        uint64_t    outerAddr  = 0;   // UObject* outer (for jump-to; 0 if none)
        uint64_t    classAddr  = 0;   // UClass*  of this object (for jump-to class)
        bool        isPawn     = false;   // is APawn subclass
        bool        isInternal = false;   // is UClass / UFunction / UEnum / UScriptStruct / UPackage
        bool        isDefault  = false;   // is Class Default Object (CDO)
        bool        isBP       = false;   // class compiled from Blueprint (CompiledFromBlueprint flag)
    };

    std::vector<Row> Snapshot(size_t maxRows = 300000);
}
