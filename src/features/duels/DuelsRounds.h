#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CrewMember;
struct ShipManager;

namespace Duels
{
    class Reader;
    class Writer;
    struct Command;

    // The match flow (roadmap 3.1, docs/design/match-flow.md): rounds of preparation and fight, best of N, the score,
    // forfeit, conceding a round and draw offers. The host's game runs the match (MSG_MATCH); the guest's shows it and
    // does the same things at the same moments. Each game still decides about its own ship: it reports its own
    // defeat and the damage its ship takes, and the host decides what that means for the round.
    namespace Rounds
    {
        static const uint8_t MSG_MATCH = 38;         // reliable, host -> guest: the match state
        static const uint8_t MSG_MATCH_EVENT = 39;   // reliable, either way: ready, defeat, forfeit, concede, draws

        enum class Phase : uint8_t
        {
            None = 0,        // no match: not connected
            Prep = 1,        // the round's preparation: repairs, scrap, the shop, upgrades; the opponent isn't there
            Starting = 2,    // both games build the other's ship; the host names the moment the fight begins
            Fight = 3,       // the fight (it begins at that moment)
            Ending = 4,      // a ship is down: its explosion runs out, shots already in the air still count
            RoundOver = 5,   // the round's result on screen
            MatchOver = 6    // the match's result
        };

        void Reset();
        void OnConnected();
        void OnDisconnected(bool opponentGone);
        void OnMessage(uint8_t type, Reader &r);
        void OnFrame(double now);

        Phase GetPhase();
        // A free fight ("match free"): one fight, no rounds, no preparation; the duel as before 3.1 (the regression tests).
        bool Free();
        // The ships meet (the loadouts go out, the opponent's ship stands): from Starting until the round is over.
        bool ShipsMeet();
        // The fight has begun: weapons charge and the opponent can be targeted.
        bool FightBegun();

        // DuelsMatch.cpp: the opponent's ship was built for this round.
        void OnReplicaBuilt(ShipManager *replica);

        // The state message carries the damage our ship took this round; the other's comes back into the score.
        void WriteState(Writer &w);
        bool ReadState(Reader &r);

        // FTL's game over (our ship destroyed, our crew dead) doesn't happen in a duel: the round is lost instead.
        bool GameOverAllowed();
        // Upgrades, buying and selling: only in the preparation (rules, section 1).
        bool ShoppingAllowed();

        // Player verbs: match [rounds <n>|prep <seconds>|permadeath on|off|free|rounds], ready, forfeit, concede,
        // draw round|match|yes|no.
        bool IsVerb(const std::string &verb);
        bool RunVerb(const Command &cmd, std::string &message);

        // The lines between the weapons bar and the subsystems: round, phase and its time, round wins, damage score.
        void Render();
        std::string Status();

        // The Duels window (DuelsWindow.cpp, roadmap 3.2): what it shows, and what its buttons may do now.
        struct Summary
        {
            bool inMatch = false;           // a match runs, or has just ended
            std::string settings;           // "best of 5 rounds, 60 s preparation, permanent death on"
            std::string state;              // "round 2 of 5: fight"
            std::string score;              // "rounds won 1 : 0, damage score 34.0 : 12.0"
            std::vector<std::string> results;
            bool canReady = false, ready = false;
            bool canConcede = false, canOfferRoundDraw = false, canOfferMatchDraw = false, canForfeit = false;
            bool drawToAnswer = false;      // the opponent offers a draw
            std::string drawText;           // about an open draw offer
        };
        Summary GetSummary();
        // A player's action, as its verb: "ready", "forfeit", "concede", "draw round", "draw match", "draw yes", "draw no".
        bool Act(const std::string &command, std::string &message);
    }
}
