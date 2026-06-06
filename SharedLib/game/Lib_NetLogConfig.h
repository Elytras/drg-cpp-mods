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
    // One entry in the 'autorun' section: a registered command name plus
    // its positional args (not including the command name itself).
    // An empty args list is equivalent to calling with State::dummyCtx.
    struct AutoRunEntry
    {
        std::string              command;
        std::vector<std::string> args;
    };

    struct Config
    {
        std::vector<std::string>  netSkip;   // unified skip list for both NetClient and NetServer loggers
        std::vector<AutoRunEntry> autorun;
    };

    // Read config.yaml using the two-path search.
    // Returns an empty Config (nothing suppressed) if the file is not found.
    Config Load();

    // The resolved config.yaml path via the two-path search; if neither exists,
    // the preferred (next-to-DLL) location so a UI can create it on first save.
    // Empty only if the module path can't be determined.
    std::string ConfigPath();

} // namespace NetLogConfig

// Execute the 'autorun' entries from config.yaml through the given handler.
// Call from LoadModsGameThread (auto-run on load) or bind to a 'runcfg' command.
void RunConfig(CommandHandler& handler);
