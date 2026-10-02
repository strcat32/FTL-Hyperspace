#pragma once

#include "DuelsRefit.h"

#include <string>
#include <vector>

struct ShipManager;

// Playing against FTL's AI (roadmap 3.6; docs/design/ai-opponent.md in the FTL: Duels repository): a local match in
// this game alone, started from HOST DUEL. The match runs as the host's does (DuelsRounds.cpp); the guest's part is the
// AI's: its ship (FTL's enemy, a player ship flown by FTL's own ship AI) comes when the ships meet, and its readiness,
// its defeat and the damage it takes come from here instead of the network. Between rounds the AI keeps its ship's
// fitting, its missiles and drone parts, its scrap and its crew (Permanent Death), and it shops by a simple rule.
// Unranked.
namespace Duels
{
    namespace Ai
    {
        // The match against the AI begins (once the run has); its ship: a player ship's blueprint, "" for a random one.
        // When the match chooses its ships (roadmap 3.9) the AI bans and picks in its turns, at random (from a host's
        // list it picks the ship given here, if the list has it), and flies its pick (TakeShip, at the reveal).
        void Start(const std::string &blueprint);
        void TakeShip(const std::string &blueprint);
        bool Active();
        // The match ends (the lobby, a new duel): the AI's ship goes.
        void Stop();
        // "AI Kestrel": the AI's name on the screen ("AI" while its ship isn't chosen).
        std::string Name();

        // Its level (roadmap DC, the user 2026-10-02) and FTL's pause (roadmap DD): HOST DUEL's choices for the next
        // match, kept in duels.cfg (ai_level easy|normal|hard, ai_pause on|off). Easy shops with 60% of the round's scrap
        // and fights to the end; Normal shops with the round's scrap and runs from a lost fight; Hard shops with 130%,
        // runs, aims at our most valuable weapon or drone, and drags a weapon or drone out of the bay our missile is
        // about to hit. Each aims at our bays (FTL's AI would aim at bay 1 only). With the pause allowed, FTL's pause
        // works as in a run, and the match's clock stands still with it (DuelsRounds.cpp).
        enum Level { EASY = 0, NORMAL = 1, HARD = 2 };
        void SetNext(int level, bool pause);
        int NextLevel();
        bool NextPause();
        const char *LevelName(int level);    // "easy", "normal", "hard" (duels.cfg, the verbs)
        const char *LevelTitle(int level);   // "Easy", "Normal", "Hard"
        int CurrentLevel();
        bool PauseAllowed();                 // in the match running now (false outside one)
        // The pause itself: the pause key (CommandGui::KeyDown) holds FTL's world (CommandGui::IsPaused) and the match's
        // clock. A match keeps FTL's own pause flags off every frame (the store, the menus, a window without focus don't
        // pause it, as in a duel), so it is the match's own.
        bool Paused();
        void TogglePause();
        // CombatAI::PrioritizeSystem for its ship (ship 1) aiming at ours: FTL picks a system; our weapons system and drone
        // control keep their weapons and drones in bays (a room each), so it aims at a bay with a weapon (a drone) in it
        // instead. The system to aim at.
        int AimAtBay(ShipManager *self, ShipManager *target, int system);

        // DuelsRounds.cpp: a round's preparation begins, with the round's scrap and stock (the AI is ready at once; it
        // shops when its ship comes); its ship is there and fitted (the ships meet; the enemy window shows it as a
        // duel's opponent); every frame of the match (the ship when the ships meet, its defeat, its damage and levels,
        // what it has left); the round is over (after Refit::EndOfRound: whose crew is where comes from FTL's own ship
        // ids here, as the duel's crew registries are the network's: boarders go home or die with a destroyed ship, and
        // the AI's crew for the next round).
        void OnPrep(int round, int scrap, const std::vector<Refit::ShopItem> &stock, bool permadeath);
        bool ShipStands();
        void OnFrame();
        void OnRoundEnd(bool ownDestroyed, bool theirsDestroyed);

        // The player ships the AI may fly (from FTL's hangar), for HOST DUEL's choice.
        std::vector<std::string> PlayerShips();
        // A player ship's name as the hangar shows it ("The Kestrel", "Kestrel B"...).
        std::string ShipTitle(const std::string &blueprint);
    }
}
