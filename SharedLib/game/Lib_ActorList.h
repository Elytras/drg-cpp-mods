#pragma once
// Lib_ActorList.h — SDK-free snapshot of live actors for the overlay's Actors tab.
//
// Snapshot() walks GObjects for AActor instances and captures a few display fields.
// It touches live UObjects, so it MUST be called on the game thread (the overlay
// schedules it via EnqueueEveryNTicks). The returned rows are plain strings/bools,
// so consumers (Lib_OverlayConsole) need no SDK headers.

//Very PoC thing so far. shouldn't be used much as a reference until better times imo.

#include <cstdint>
#include <string>
#include <vector>

namespace ActorList
{
    struct Row
    {
        std::string className;
        std::string name;
        std::string outer;        // UObject Outer (usually the level/world)
        std::string owner;        // empty if none / not replicated-relevant
        std::string instigator;   // empty if none
        bool        replicated = false;
        // Slash-wrapped class ancestry incl. self ("/Self/Parent/.../Object/") so the
        // UI can do subclass filtering with a plain substring test, no SDK access.
        std::string classChain;
        // Object pointers as integers — unique identity (GetName() collides across
        // distinct instances). Used for selection/highlight/jump, not display.
        uint64_t    addr = 0, ownerAddr = 0, instigatorAddr = 0, outerAddr = 0;
    };

    // Build a snapshot of current actors (game thread only). Capped at maxRows to
    // bound cost on huge worlds.
    std::vector<Row> Snapshot(size_t maxRows = 8000);
}
