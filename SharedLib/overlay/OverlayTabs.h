// OverlayTabs.h — internal plumbing shared between the overlay tab TUs.
//
// This is NOT the public OverlayConsole API (that's Lib_OverlayConsole.h). It
// declares the small set of services each per-tab TU (overlay/tabs/*.cpp) needs
// from the overlay core (Lib_OverlayConsole.cpp), plus the tab-registry base.
//
// Layer: game (Layer 3) — pulls VarSystem / ActorList SDK types; compiled per game.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../core/GameThreadSnapshot.h"  // GameThreadSnapshot<T> (snapshot accessor return types)
#include "../game/Lib_VarSystem.h"       // VarSystem::VarType (VarSnap)
#include "../game/Lib_ActorList.h"       // ActorList::Row (Actors() snapshot)
#include "../game/Lib_ObjectList.h"      // ObjectList::Row (Objects() snapshot)
#include "ObjectView.h"                  // ObjView::ObjectView / Request (Objects property tree)

class CommandHandler;

namespace OverlayConsole
{
    // Services the tab TUs consume. All defined once in Lib_OverlayConsole.cpp
    // (the overlay core); external linkage, so the per-tab TUs can call them.
    namespace detail
    {
        // Echo `line` into the log pane and dispatch it on the game thread via the
        // bound handler. Safe to call from the overlay (UI) thread. Used by every
        // tab that runs a command.
        void RunCommand(std::string line);

        // The bound command handler (null until Init binds it). The Commands tab and
        // completion engine read it directly.
        CommandHandler* Handler();

        // Steady-clock milliseconds — snapshot heartbeats / due() windows.
        uint64_t NowMs();

        // ── Game-thread → UI snapshots ───────────────────────────────────────
        // Produced by the Init() tick (game thread); read by the tabs (UI thread).
        // VarSnap is the overlay's flattened view of one VarSystem entry.
        struct VarSnap { std::string name, token; VarSystem::VarType type; };
        GameThreadSnapshot<std::vector<VarSnap>>&        Vars();
        GameThreadSnapshot<std::vector<ActorList::Row>>&  Actors();
        GameThreadSnapshot<std::vector<ObjectList::Row>>& Objects();

        // Objects-tab property tree: the focused object's model (game thread builds it every
        // tick while the tab is live; UI renders it) + the UI→game request (focus + expanded
        // node keys). The UI beats ObjectView() each frame it renders the tree so the producer
        // only works while the tab is actually shown.
        GameThreadSnapshot<ObjView::ObjectView>& ObjectViewSnap();
        void                SetObjViewRequest(const ObjView::Request& req);   // UI → game
        ObjView::Request    GetObjViewRequest();                              // game reads

#if defined(RogueCore) && RogueCore
        GameThreadSnapshot<std::vector<std::string>>& Negotiations();
#endif

        // ── Tab registry ─────────────────────────────────────────────────────
        // Constructing an OverlayTab<Derived> self-registers a {name, draw} entry
        // (type-erased — no virtuals on the tab). DrawPanel iterates the registry
        // in registration order. Derived must provide `static constexpr char* kName`
        // and `void Draw()`.
        struct OverlayTabEntry { const char* name; std::function<void()> draw; };
        std::vector<OverlayTabEntry>& Tabs();

        template<class Derived>
        struct OverlayTab
        {
            OverlayTab()
            {
                Tabs().push_back({ Derived::kName,
                                   [self = static_cast<Derived*>(this)] { self->Draw(); } });
            }
        };

        // Per-tab registration entry points — each defined in its own TU and called
        // from Init() in display order. Each constructs a session-lifetime instance
        // (which self-registers via OverlayTab<>). Calling in order fixes the tab
        // display order deterministically across TUs.
        void RegisterConsoleTab();
        void RegisterCommandsTab();
        void RegisterVarsTab();
        void RegisterKeybindsTab();
        void RegisterObjectsTab();   // merged Actors + Objects browser
        void RegisterLogsTab();
        void RegisterConfigTab();
#if defined(RogueCore) && RogueCore
        void RegisterNegotiationTab();
        void RegisterResourceTab();
#endif
    }
}
