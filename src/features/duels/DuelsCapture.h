#pragma once

#include <string>

// Screenshots for tests, read from the frame FTL draws (graphics_read_pixels), so they work whatever the window shows:
// covered by other windows, or with the computer locked.
namespace Duels
{
    // Saves the next frame as a 24-bit BMP; the log says when it is written. FTL draws its frames into a 1280 x 720
    // frame buffer and scales that to the window: that frame buffer is what is saved (the window itself if FTL draws
    // straight into it).
    void RequestCapture(const std::string &path);
}
