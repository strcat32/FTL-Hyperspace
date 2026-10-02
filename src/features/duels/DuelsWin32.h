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

    // Test runs can keep the games off the user's main monitor: the runners set DUELS_DISPLAY ("x,y,width,height" of
    // another monitor) and DUELS_DISPLAY_SCALE (two 1280 x 720 games side by side don't fit on a 1920-wide monitor).
    // Window positions then count from that monitor's corner, and positions and sizes are scaled; without a size, the
    // window gets FTL's 1280 x 720 scaled. Returns false when no test display is set (nothing changed).
    bool MapToTestDisplay(int &x, int &y, int &width, int &height);

    // DUELS_WINDOW ("x,y", mapped as above) places the window on the test display as soon as it exists, before the
    // game's menu comes up. Call every frame; it acts once.
    void PlaceOnTestDisplay();

    bool SetGameWindowTitle(const char *title, std::string &details);

    // A file written in full takes another's place in one step (a crash never leaves half of one: DuelsRejoin.cpp).
    bool PutFileInPlace(const std::string &from, const std::string &to);
}
