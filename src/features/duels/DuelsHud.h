#pragma once

// Things FTL: Duels draws over the game's interface. Hooks live in DuelsHooks.cpp and call in here.
namespace Duels
{
    namespace Hud
    {
        // The network numbers (verb "netstats on|off"): three short lines at the top right, under the version label,
        // in its tiny font. Round trip, lost packets and resends, traffic each way, and how we're connected.
        void SetNetStats(bool on);
        bool NetStats();

        // Late in the frame (MouseControl::OnRender), before the console and the mouse pointer.
        void Render();

        // Test verb "fonttest [seconds]": the match display's text in each of FTL's fonts, over the game.
        void FontTest(double seconds);
    }
}
