#pragma once
// ObjectView.h — SharedLib · overlay model (SDK-free).
//
// A passive, owned snapshot of one inspected UObject's property/function tree, built on the
// GAME THREAD by Lib_ObjectView and rendered by the Objects tab on the UI thread. The whole
// point of the refactor (see plans/ObjectsTab refactor): the UI renders THIS and never
// dereferences a live UObject*/FProperty*/array/FText — every live read happens game-side under
// validity checks, so the GC/realloc/free crash class is gone.
//
// All SDK references are opaque integers (addresses cast to uint64) the UI only passes back to
// the game thread (jumps, path-resolved writes); the UI must never reinterpret them as pointers.

#include <cstdint>
#include <string>
#include <vector>

namespace ObjView
{
    using uint8  = unsigned char;
    using int32  = signed int;
    using int64  = signed long long;
    using uint64 = unsigned long long;

    // How the UI should render/edit a leaf value.
    enum class EditKind : uint8
    {
        ReadOnly,    // text only (maps/sets, unhandled types)
        Bool,        // checkbox
        Int,         // DragInt
        Float,       // DragFloat
        Double,      // DragScalar(double)
        Name,        // text field (FName)
        Str,         // text field (FString)
        Text,        // text field (FText)
        Enum,        // combo (enumNames/enumValues)
        VectorLike,  // DragFloat3 (Vector* / Rotator)
        Object,      // read-only label + jump button
    };

    // Container kind for a node (drives expansion + the "+ append" affordance).
    enum class Container : uint8 { None, Array, Set, Map };

    // ── Property path ────────────────────────────────────────────────────────────
    // A leaf's address expressed as hops FROM the validated root object, re-walked on the game
    // thread at write time (re-reading each container's live data pointer) — this is what makes
    // writes realloc-safe and unblocks nested containers (the old "N-hop" problem). The UI holds
    // a PropPath opaquely and passes it back to EnqueuePathWrite; it never dereferences leafProp.
    struct PropHop
    {
        enum class Kind : uint8 { Struct, ArrayElem } kind = Kind::Struct;
        int32 offset   = 0;   // Struct: field offset added to base.
                              // ArrayElem: offset of the TArray header within the current base.
        int32 index    = 0;   // ArrayElem: element index.
        int32 elemSize = 0;   // ArrayElem: inner element stride.
    };

    struct PropPath
    {
        uint64 rootAddr   = 0;    // the inspected object (validated by index before any walk)
        int32  rootIndex  = -1;   // UObject::Index recorded at build time
        std::vector<PropHop> hops;   // root → the container/struct holding the leaf
        uint64 leafProp   = 0;    // FProperty* of the leaf (opaque; game-thread reinterpret only)
        bool   leafIsArrayElem = false;  // last hop is an ArrayElem leaf → grow/append capable
    };

    // ── Property node ──────────────────────────────────────────────────────────────
    struct PropNode
    {
        std::string name;
        std::string typeName;     // struct/elem type label
        std::string valueStr;     // formatted value (also the edit seed)
        EditKind    edit = EditKind::ReadOnly;
        int         depth = 0;
        uint64      key = 0;      // stable expansion key (UI toggles it into Request.expanded)
        PropPath    path;         // how to write/locate this leaf on the game thread

        // Type extras (only the ones relevant to `edit` are populated):
        bool        boolVal  = false;          // Bool
        float       vec3[3]  = { 0, 0, 0 };    // VectorLike (read as float; double-backed on UE5)
        int64       enumValue = 0;             // Enum: current backing value
        int32       enumSize  = 0;             // Enum: backing-int width
        std::vector<std::string> enumNames;    // Enum: entry labels
        std::vector<int64>       enumValues;   // Enum: parallel values
        uint64      objectAddr = 0;            // Object: jump target (0 = null/invalid)

        // Container/expansion:
        Container   container = Container::None;
        int32       containerNum = 0;          // element count (Array/Set/Map)
        bool        hasChildren = false;       // struct/expandable container
        bool        expanded = false;          // mirrors the UI's request for this node
        std::vector<PropNode> children;        // populated only when expanded
    };

    // ── Function node ────────────────────────────────────────────────────────────
    struct ParamView
    {
        std::string name;
        std::string type;
        int         dir = 0;   // 0 in, 1 pure-out, 2 return, 3 in-out
        // Edit metadata so the arg builder renders the same typed widgets as the property tree.
        EditKind    edit = EditKind::ReadOnly;
        int32       enumSize = 0;
        std::vector<std::string> enumNames;
        std::vector<int64>       enumValues;
    };

    struct FuncView
    {
        std::string name;
        std::string suffix;          // " [BP]"/" [Native]"… flags
        std::vector<ParamView> params;
        bool        invokable = false;
        uint64      funcId = 0;       // UFunction* (opaque; game-thread reinterpret only)
    };

    // ── Class level + whole-object view ─────────────────────────────────────────
    struct ClassLevelView
    {
        std::string className;
        int         level = 0;       // 0 = most-derived (drives the color palette)
        bool        expanded = true;
        std::vector<PropNode> props;
        std::vector<FuncView> funcs;
    };

    struct ObjectView
    {
        uint64 addr  = 0;
        int32  index = -1;
        bool   valid = false;        // false → object was destroyed/replaced; UI shows a notice
        std::string objName;
        std::string className;
        std::string outerName;
        uint64 outerAddr = 0;
        uint64 classAddr = 0;
        std::vector<ClassLevelView> chain;
    };

    // ── UI → game request channel ──────────────────────────────────────────────
    // The UI publishes the focus object + which nodes are expanded; the game-thread producer
    // reads it each tick and rebuilds `ObjectView` for the focused object (fast-path). Path-keys
    // identify expanded nodes stably across rebuilds (see Lib_ObjectView for the key scheme).
    struct Request
    {
        uint64 focusAddr  = 0;
        int32  focusIndex = -1;
        std::vector<uint64> expanded;   // sorted path-keys of expanded nodes
    };
}
