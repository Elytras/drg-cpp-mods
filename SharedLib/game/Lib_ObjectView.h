#pragma once
// Lib_ObjectView.h — SharedLib · game (Layer 3): builds the SDK-free ObjView::ObjectView model
// for the Objects tab, on the GAME THREAD. The overlay renders the returned snapshot and never
// touches live game memory (see plans/ObjectsTab refactor). Reuses BuildClassChain /
// GetFieldValueAsString / PropertyAccess; produces PropPath addressing so writes/jumps re-resolve
// on the game thread.

#include "../overlay/ObjectView.h"

namespace SDK { class UObject; }

namespace ObjView
{
    // Build the full view for `obj`, validating it (IsValidRaw + Index) first. Children are
    // expanded only for nodes whose path-key is present in req.expanded (bounded breadth via the
    // shared kMaxArrShow cap). Returns {valid=false} if the object is gone/replaced.
    ObjectView BuildObjectView(SDK::UObject* obj, const Request& req);

    // Stable per-node key (matches the keys the builder emits) so the UI can toggle expansion
    // without holding the model: childKey(parentKey, discriminator).
    uint64 ChildKey(uint64 parentKey, uint64 discriminator);
}
