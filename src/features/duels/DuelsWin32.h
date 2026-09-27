#pragma once

#include <string>

// Win32-only helpers, kept out of files that include Global.h because <windows.h> macros clash with FTL's headers.
namespace Duels
{
    // Both return false when the game window was not found; `details` describes the window and its state.
    bool MinimizeGameWindow(std::string &details);
    bool RestoreGameWindow(std::string &details);
}
