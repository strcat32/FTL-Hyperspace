#pragma once

// Things FTL: Duels draws over the game's interface. Hooks live in DuelsHooks.cpp and call in here.
namespace Duels
{
    namespace Hud
    {
        // The network numbers (verb "netstats on|off"): three short lines at the bottom left, next to the reactor
        // bar and above the systems. Round trip, lost packets and resends, traffic each way, and how we're connected.
        void SetNetStats(bool on);
        bool NetStats();

        // Late in the frame (MouseControl::OnRender), before the console and the mouse pointer.
        void Render();
    }
}
