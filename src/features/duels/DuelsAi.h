#pragma once

#include "DuelsRefit.h"

#include <string>
#include <vector>

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
        void Start(const std::string &blueprint);
        bool Active();
        // The match ends (the lobby, a new duel): the AI's ship goes.
        void Stop();
        // "AI Kestrel": the AI's name on the screen.
        std::string Name();

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
