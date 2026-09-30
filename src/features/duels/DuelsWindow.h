#pragma once

#include <string>
#include <vector>

namespace Duels
{
    // The Duels button next to FTL's options button, and the window it opens (roadmap 3.2): the opponent and the room,
    // the match's settings, state, score and results, how to win, the icons Duels adds, and the players' actions
    // (ready, concede the round, offer a draw for the round or the match, answer an offer, forfeit).
    namespace Window
    {
        // Over the game (MouseControl::OnRender): the button, and the window while it is open.
        void Render();
        // CommandGui::MouseMove, LButtonDown, KeyDown: true when the button or the window took it (FTL doesn't see
        // it then). Escape closes the window.
        bool MouseMove(int x, int y);
        bool LButtonDown(int x, int y);
        bool KeyDown(int key);
        bool IsOpen();
        // Test verb: duels open|close | duels click <x> <y> (a left click there, in FTL's 1280 x 720 coordinates).
        bool RunVerb(const std::vector<std::string> &args, std::string &message);
    }
}
