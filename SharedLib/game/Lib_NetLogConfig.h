#pragma once
#include <string>
#include <vector>

class CommandHandler; // forward-declared to avoid pulling Lib_CommandHandler.h into every TU

// ============================================================================
// Lib_NetLogConfig — loads per-command skip lists from config.yaml.
//
// Searched in two locations (first match wins):
//   1. Next to the module DLL   — distribution / in-game use
//   2. Two directories above    — development (next to the solution file)
//
// Changes take effect the next time a log command is enabled:
// disable → edit file → re-enable (or run 'reloadnetlog').
// ============================================================================

namespace NetLogConfig
{
    struct Config
    {
        std::vector<std::string> netSkip;   // unified skip list for both NetClient and NetServer loggers
    };

    // Read config.yaml's netlog section using the two-path search.
    // Returns an empty Config (nothing suppressed) if the file is not found.
    Config Load();

    // The resolved config.yaml path via the two-path search; if neither exists,
    // the preferred (next-to-DLL) location so a UI can create it on first save.
    // Empty only if the module path can't be determined.
    std::string ConfigPath();

    // The resolved autorun script path. Honors the `autorun` cvar if it names an
    // existing file; otherwise the two-path search for `autorun.cfg`, falling back
    // to the preferred (next-to-DLL) location for a first save. Empty only if the
    // module path can't be determined.
    std::string AutorunPath();

} // namespace NetLogConfig

// Execute the autorun script (one console command per line; blank lines and lines
// starting with '#' or '//' are skipped) through the given handler. Call from
// LoadModsGameThread (auto-run on load) or bind to a 'runcfg' command.
void RunConfig(CommandHandler& handler);
