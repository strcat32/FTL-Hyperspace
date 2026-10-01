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
            MatchOver = 6,   // the match's result
            Choice = 7       // before round 1: the ships are chosen, by bans and a pick (roadmap 3.9)
        };

        void Reset();
        void OnConnected();

        // A match against FTL's AI (DuelsAi.cpp, roadmap 3.6): this game hosts it alone and nothing goes over the
        // network; the guest's part (its readiness, its defeat, the damage its ship takes and its hull and crew now, as
        // shares) comes from the AI.
        void StartLocal();
        bool IsLocal();
        void OpponentReady();
        void OpponentDefeated(bool crewDead);
        void OpponentState(float hullLost, float crewLost, float hullShare, float crewShare);
        // A lost connection pauses the match (roadmap AA; rules, section 3): FTL's world stands still and the ships take
        // no orders until both players are back; the phase, stall and draw timers and the environment wait too.
        void OnConnectionLost();
        bool NetPaused();
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
        // A match runs and is in a round's preparation (ShoppingAllowed is also true outside a match).
        bool InPreparation();

        // Running away (roadmap AD; rules, section 3): in a match's fight the FTL drive charges as in FTL, and its jump
        // ends the round, half a point for the other player and none for the runner. InMatch: a match runs (FTL's
        // star map stays shut); EscapeAllowed: this is its fight and our ship isn't down; DriveReady: FTL's own terms
        // for a jump (the drive charged, the engines and piloting working); Escape: the jump, as the JUMP button's.
        bool InMatch();
        bool EscapeAllowed();
        bool DriveReady();
        bool Escape(std::string &message);

        // Player verbs: match [rounds <n>|prep <seconds>|permadeath on|off|ships own|bans|list <ships>|pool <types>|
        // free|rounds], ready, forfeit, concede, draw round|match|yes|no|back (a draw offer stands until it is answered
        // or taken back, or the round ends), ban <type>, pick <n> (the ship choice).
        bool IsVerb(const std::string &verb);
        bool RunVerb(const Command &cmd, std::string &message);

        std::string Status();

        // The host's settings for the next duel, as the Host window (DuelsLobby.cpp, roadmap 3.5) shows and changes
        // them; changed as the "match" verbs change them (kept in duels.cfg in a player's game). Not while a match runs.
        struct NextDuel
        {
            int rounds = 5;
            int prepSeconds = 60;
            int stallSeconds = 300;
            bool permadeath = true;
            uint8_t env = 0;        // Environment::Mode
            uint8_t hazards = 0;    // the kinds MODE_AUTO may roll
            bool record = true;     // public recording (off: unranked)
        };
        NextDuel GetNextDuel();
        bool SetNextDuel(const NextDuel &next, std::string &message);

        // The Duels window (DuelsWindow.cpp, roadmap 3.2): what it shows, and what its buttons may do now.
        struct Summary
        {
            bool inMatch = false;           // a match runs, or has just ended
            // The window's rows (AS): each a short line, the same rows whatever the match.
            std::vector<std::string> rules; // the settings (the next duel's outside a match): "Best of 5 rounds, 60 s
                                            // preparation", "Permanent death on", "No progress for 5 min ends a round",
                                            // "No hazards", "Ranked" (or "Unranked: " and why)
            std::string state;              // "round 2 of 5: fight"
            std::string score;              // "points 1 : 0, damage score 34.0 : 12.0"
            std::string fight;              // "This round: near a sun"
            std::string tally;              // "Rounds: 2 won, 1 lost, 0 drawn"
            std::string lastRound;          // "Last round: you won (ship destroyed)"
            std::vector<std::string> results;   // each round's, in full (the end screen)
            bool canReady = false, ready = false;
            bool canConcede = false, canOfferRoundDraw = false, canOfferMatchDraw = false, canForfeit = false;
            bool drawToAnswer = false;      // the opponent offers a draw
            std::string drawText;           // about an open draw offer

            // The match on screen (DuelsMatchUi.cpp, roadmap R, AB, AC).
            Phase phase = Phase::None;
            int round = 0, rounds = 0;
            bool free = false;
            uint8_t me = 0;                 // 0: we host (red), 1: we joined (blue)
            std::string names[2];           // the host's (red) and the guest's (blue)
            std::string points[2];          // "1.5"
            bool opponentReady = false;
            bool weOfferDraw = false;       // our draw offer is open (the round's or the match's)
            bool drawIsMatch = false;       // the open offer is for the match
            bool canUnready = false;
            std::string envName;            // the next fight's environment in the preparation ("a sun"), or ""
            // The timer that runs now: its label and the time left (ms); msLeft < 0: none.
            std::string countdownLabel;
            double countdownMs = -1.0;
            bool paused = false;            // the connection is lost: the match waits
            std::string pausedText;         // "Waiting for Captain_Lil" / "Getting back into the match"

            // The ship choice (Phase::Choice, roadmap 3.9).
            struct Choice
            {
                bool bans = false;              // bans, then a pick (else a pick from the host's list)
                uint16_t pool = 0;              // the types in the bans (bits by Ships' type)
                std::vector<uint8_t> banned;    // the types banned, in order
                std::vector<uint8_t> bannedBy;  // who banned each: 0 the host, 1 the guest
                std::vector<bool> byServer;     // time ran out: the server banned it
                int bansTotal = 0;
                uint8_t banner = 2;             // whose ban it is now (2: nobody's)
                std::vector<std::string> offer; // the ships to pick from (blueprints), once the bans are done
                bool picked[2] = {false, false};
                int ourPick = -1;               // the offer's index we picked
                std::string ships[2];           // each player's ship, from the reveal on
            } choice;
        };
        Summary GetSummary();
        // A player's action, as its verb: "ready", "ready off", "forfeit", "concede", "draw round", "draw match",
        // "draw yes", "draw no", "draw back".
        bool Act(const std::string &command, std::string &message);
    }
}
