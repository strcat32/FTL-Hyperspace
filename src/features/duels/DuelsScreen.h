#pragma once

#include <string>

struct GL_FrameBuffer;

// FTL's picture: how finely it is drawn, screenshots of it, and the name it shows.
//
// FTL draws every frame into a 1280 x 720 frame buffer and scales that to the window. Anything drawn smaller than its
// size (the opponent's ship in a duel) loses detail twice then: when it is drawn into the frame buffer, and when that
// is scaled up. So when the window is larger and not an exact multiple of 1280 x 720 (where FTL scales sharply by
// whole pixels), frames are drawn into a frame buffer of the window's size instead, still in FTL's 1280 x 720
// coordinates, with smooth filtering: FTL's own look stays (it scales smoothly there too), and shrunk ships keep their
// detail. ("hires", on by default.)
namespace Duels
{
    namespace Screen
    {
        void SetHiRes(bool on);
        bool HiRes();
        std::string Describe();

        // Saves the next frame as a 24-bit BMP, read from the frame buffer (graphics_read_pixels), so it works with
        // the window covered or the computer locked; the log says when it is written.
        void RequestCapture(const std::string &path);

        // The name at the top right ("FTL:Duels <version>" instead of Hyperspace's "HS-<version>") and in the window
        // title.
        bool VersionLabel(int fontSize, float x, float y, const std::string &text, std::string &label);
        // The version label's font (tiny; the network numbers use it too), or -1 before it was drawn.
        int VersionFont();
        void OnFrame();
    }
}
