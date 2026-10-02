#pragma once

#include <string>
#include <vector>

// The player's own record of their matches (roadmap BR, the user 2026-10-02: STATS in the main menu shows the online
// play's stats): duels-stats.txt in the game's folder, a line for each finished match against another player (not
// against the AI, not a replay), read for the STATS window (DuelsMenu.cpp).
namespace Duels
{
    namespace Stats
    {
        struct MatchLine
        {
            std::string when;                     // the local time it ended, "2026-10-02 14:03"
            bool ranked = false;
            bool host = true;                     // this player hosted
            std::string name, opponent;
            std::string ship, opponentShip;       // blueprints ("PLAYER_SHIP_HARD")
            int result = 0;                       // 1 won, 0 drawn, -1 lost
            int halves = 0, opponentHalves = 0;   // the points, doubled (a drawn round is half a point)
            int rounds = 0;
            double damage = 0.0, opponentDamage = 0.0;   // the damage scores: what each dealt
            int seconds = 0;
            std::string how;                      // how it ended ("more points", "forfeit", "left the match", ...)
        };

        // A finished match: a line at the end of the file.
        void Record(const MatchLine &line);
        // All of them, the oldest first (lines of other versions that don't read are left out).
        std::vector<MatchLine> Load();
    }
}
