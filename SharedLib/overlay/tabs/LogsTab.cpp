// LogsTab.cpp — overlay "Logs" tab: checkboxes for the togglable loggers.
//
// Layer: game (Layer 3). Registered via detail::RegisterLogsTab() (called from
// OverlayConsole::Init in display order). The toggle list + live state come from
// SharedCommands (GetLogToggles); flipping a box runs the toggle's CLI command on
// the game thread via RunCommand — the same path as typing the command.

#include "../OverlayTabs.h"
#include "../Lib_OverlayUI.h"
#include "../../game/SharedCommands.h"   // GetLogToggles / LogToggleInfo

#include <imgui.h>

namespace OverlayConsole
{
    namespace
    {
        using detail::RunCommand;

        struct LogsTab : detail::OverlayTab<LogsTab>
        {
            static constexpr const char* kName = "Logs";

            void Draw()
            {
                UI::TextDisabled("Live ProcessEvent loggers. Output streams to the Console "
                                 "pane and the CLI.");
                UI::Spacing();

                for (const auto& t : GetLogToggles())
                {
                    // State is read live each frame; the checkbox click runs the toggle
                    // command (game thread), so the box reflects reality next frame even
                    // if the toggle was flipped from the CLI or autorun.
                    bool on = t.enabled ? t.enabled() : false;
                    if (UI::Checkbox(t.label, &on))
                        RunCommand(t.id);
                    if (UI::IsItemHovered() && t.desc)
                        UI::SetTooltip("%s  (cmd: %s)", t.desc, t.id);
                }

                UI::SectionHeader("Filtered watch");
                UI::TextDisabled("From the Console: 'pewatch <kw> [kw...]' (keyword PE log "
                                 "with args), 'netfreq [client|server|all]' (RPC frequency table).");
            }
        };
    } // anonymous namespace

    void detail::RegisterLogsTab() { static LogsTab s; (void)s; }
}
