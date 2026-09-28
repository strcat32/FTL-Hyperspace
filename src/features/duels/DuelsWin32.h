#pragma once

#include <string>

// Win32-only helpers, kept out of files that include Global.h because <windows.h> macros clash with FTL's headers.
namespace Duels
{
    // All return false when the game window was not found; `details` describes the window and its state.
    bool MinimizeGameWindow(std::string &details);
    bool RestoreGameWindow(std::string &details);

    // Moves the window's top-left corner to screen position x, y (two games side by side for tests).
    bool MoveGameWindow(int x, int y, std::string &details);

    // Saves what the game window shows (even when covered by other windows) as a 24-bit BMP.
    bool CaptureGameWindow(const std::string &path, std::string &details);
}
