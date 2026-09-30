#pragma once

#include <string>
#include <vector>

namespace Duels
{
    // The DUELS button next to FTL's options button, and the window it opens (roadmap 3.2, its look S): the opponent
    // and the room, the match's settings, state, score and results, how to win, the icons Duels adds, and the players'
    // actions (ready, concede the round, offer a draw for the round or the match, answer an offer, forfeit). The
    // button wears FTL's STORE button frame and shakes with FTL's top bar; the window FTL's window outline and title
    // tab over FTL: Duels' red and blue (DuelsStyle.cpp).
    namespace Window
    {
        // Over the game (MouseControl::OnRender): the button, and the window while it is open.
        void Render();
        // The DUELS button's right edge (its frame's visible part): the enemy window keeps below the top buttons left
        // of it (DuelsView.cpp). 0 outside the game.
        float ButtonsRight();
        // CommandGui::MouseMove, LButtonDown, KeyDown: true when the button or the window took it (FTL doesn't see
        // it then). Escape closes the window.
        bool MouseMove(int x, int y);
        bool LButtonDown(int x, int y);
        bool KeyDown(int key);
        bool IsOpen();
        // Opens it (the menu's HOST DUEL and JOIN DUEL, once the run began: the room's code is there); closes it (the end
        // screen comes over the game).
        void Open();
        void Close();
        // Test verb: duels open|close | duels click <x> <y> (a left click there, in FTL's 1280 x 720 coordinates) |
        // duels press <action> (a click on the window's button for that action: ready, concede, draw round, ...).
        bool RunVerb(const std::vector<std::string> &args, std::string &message);
    }
}
