#pragma once

#include <string>

// Win32-only helpers, kept out of files that include Global.h because <windows.h> macros clash with FTL's headers.
namespace Duels
{
    // All return false when the game window was not found; `details` describes the window and its state.
    bool MinimizeGameWindow(std::string &details);
    bool RestoreGameWindow(std::string &details);

    // Moves the window's top-left corner to screen position x, y (two games side by side for tests); with a
    // width and height > 0, also sizes its drawing area.
    bool MoveGameWindow(int x, int y, int width, int height, std::string &details);
}
