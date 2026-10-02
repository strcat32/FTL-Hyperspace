#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The player's own record of their matches (roadmap BR, the user 2026-10-02: STATS in the main menu shows the online
// play's stats): duels-stats.txt in the game's folder, a line for each finished match (against another player or the
// AI; not a replay), read for the STATS window (DuelsMenu.cpp: ranked, unranked and against the AI apart, roadmap CX).
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
            bool ai = false;                      // against the AI (never ranked)
        };

        // A finished match: a line at the end of the file.
        void Record(const MatchLine &line);
        // STATS' CLEAR (roadmap CX): the unranked matches against players, or those against the AI, go from the file;
        // the ranked ones stay. How many went.
        int Clear(bool ai);

        // The master's statistics (roadmap CJ; docs/design/stats.md in the FTL: Duels repository): what this game bought,
        // round by round (its ship before a preparation against after it), and at the match's end its side of the match
        // to the master (POST /api/summary): the ship, the result, rounds, length, damage, what it bought. Not for a
        // match against the AI, a replay, a duel in debug mode or a test run; `send_stats off` in duels.cfg: never.
        void PrepStarted(bool firstRound);
        void PrepEnded();
        void SendSummary(const MatchLine &line, uint64_t matchToken);
        // All of them, the oldest first (lines of other versions that don't read are left out).
        std::vector<MatchLine> Load();
    }
}
