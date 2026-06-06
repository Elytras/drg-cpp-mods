// Lib_ActorList.cpp — game-thread actor enumeration for the overlay Actors tab.
//
// Compiled once per game DLL against that game's SDK (like the other SharedLib
// .cpp). AActor's Owner/Instigator/bReplicates are emitted as raw pointers /
// bitfield by Dumper-7 in BOTH DRG (UE4) and RC (UE5), so this single source
// compiles for both.

#include "Lib_ActorList.h"
#include "Lib_ObjectCast.h"   // IsValid
#include "Lib_Forward.h"

#include <unordered_map>

using namespace SDK;

namespace ActorList
{
    static std::string SafeName(UObject* o)
    {
        return (o && IsValid(o)) ? o->GetName() : std::string{};
    }

    // "/Self/Parent/.../Object/" — walk UStruct::SuperStruct from the actor's class.
    static const std::string& ClassChain(UClass* cls, std::unordered_map<UClass*, std::string>& cache)
    {
        auto it = cache.find(cls);
        if (it != cache.end()) return it->second;
        std::string chain = "/";
        for (UStruct* s = cls; s; s = s->SuperStruct) { chain += s->GetName(); chain += '/'; }
        return cache.emplace(cls, std::move(chain)).first->second;
    }

    std::vector<Row> Snapshot(size_t maxRows)
    {
        std::vector<Row> out;
        if (!UObject::GObjects) return out;

        UClass* actorClass = AActor::StaticClass();
        if (!actorClass) return out;

        std::unordered_map<UClass*, std::string> chainCache;
        const int num = UObject::GObjects->Num();
        out.reserve(1024);
        for (int i = 0; i < num && out.size() < maxRows; ++i)
        {
            UObject* obj = UObject::GObjects->GetByIndex(i);
            if (!obj || !IsValid(obj) || obj->IsDefaultObject()) continue;
            if (!obj->IsA(actorClass)) continue;

            auto* actor = static_cast<AActor*>(obj);
            Row r;
            r.className  = obj->Class ? obj->Class->GetName() : "?";
            r.name       = obj->GetName();
            r.replicated = actor->bReplicates;
            r.outer      = SafeName(obj->Outer);
            r.owner      = SafeName(actor->Owner);
            r.instigator = SafeName(actor->Instigator);
            r.addr           = (uint64_t)obj;
            r.outerAddr      = (uint64_t)obj->Outer;
            r.ownerAddr      = (uint64_t)actor->Owner;
            r.instigatorAddr = (uint64_t)actor->Instigator;
            if (obj->Class) r.classChain = ClassChain(obj->Class, chainCache);
            out.push_back(std::move(r));
        }
        return out;
    }
}
