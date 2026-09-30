#pragma once

#include <string>

// The match on screen, second version (roadmap R, AB, AC; docs/design/match-ui.md in the FTL: Duels repository): the
// score panel under the scrap, the countdown, the match's buttons (Ready, Draw, Concede) and the splashes that give the
// match a fighting game's feel ("ROUND 2", "FIGHT!", "YOU WIN"). The match itself is DuelsRounds.cpp; this only shows it
// and turns clicks into its verbs.
namespace Duels
{
    namespace MatchUi
    {
        enum Colour
        {
            WHITE = 0,
            RED = 1,     // the host's
            BLUE = 2,    // the guest's
            GOLD = 3
        };

        // Everything but the splashes (drawn under the Duels window), and the splashes (drawn over everything).
        void Render();
        void RenderSplash();
        // The buttons take their clicks before the game does; true when one did.
        bool LButtonDown(int x, int y);
        void MouseMove(int x, int y);

        // A splash across the middle of the screen: big letters in a colour for a moment, with one of FTL's sounds
        // (sounds.xml names, "" for none); withNames adds both players' names under it, in red and blue.
        void Splash(const std::string &text, Colour colour, double ms, const std::string &sound, bool withNames = false);
    }
}
