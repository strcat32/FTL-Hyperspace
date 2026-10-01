#pragma once

#include <string>
#include <vector>

// The demo browser (roadmap AU; docs/design/demos.md, stage 5, in the FTL: Duels repository): REPLAYS in the title
// screen's Duels panel opens a window with the demos in demos\, a row each: the file, the date, the host's and the
// guest's names, ranked or not, both ships, the length. A click on a column's head sorts by it (again: the other way
// round); a click on a row marks it, PLAY (or a second click on it) plays it: a run starts as HOST DUEL's START starts
// one, then the replay (DuelsLobby.cpp).
namespace Duels
{
    namespace ReplayList
    {
        void Open();
        void Close();
        bool IsOpen();
        void Render();
        // Over the main menu: while it is open it takes the menu's input.
        bool MouseClick(int x, int y);
        void MouseMove(int x, int y);
        bool KeyDown(int key);
        // Test verb: menu replays [close | sort <column> | pick <row> | play [<file>|newest]].
        bool RunVerb(const std::vector<std::string> &args, std::string &message);
    }
}
