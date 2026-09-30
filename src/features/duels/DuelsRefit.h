#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

struct ShipManager;

namespace Duels
{
    // Our ship between the rounds of a match (DuelsRounds.cpp, docs/design/match-flow.md): what FTL does at the end
    // of a fight is replaced by the duel's own round end, and each preparation starts with a whole ship.
    namespace Refit
    {
        // An item of a round's shop. The host picks the stock (rules, section 7) and sends it; both games build the
        // same store from it (Hyperspace's custom store).
        enum Kind : uint8_t
        {
            KIND_WEAPON = 0,
            KIND_DRONE = 1,
            KIND_AUGMENT = 2,
            KIND_CREW = 3,
            KIND_SYSTEM = 4,
            KIND_MISSILES = 5,      // a resource: count items at price each (0 = FTL's own price)
            KIND_DRONE_PARTS = 6
        };

        struct ShopItem
        {
            uint8_t kind = KIND_WEAPON;
            std::string blueprint;
            uint16_t price = 0;
            uint8_t count = 1;
        };

        // The host: a round's stock. Weapons first, then two or three of drones, augments, systems and crew
        // (three sections in rounds 1-2, four later), three items each, drawn as FTL's stores draw them (weight
        // 6 - rarity) within the round's price cap for weapons and drones; missiles and drone parts.
        std::vector<ShopItem> MakeStock(int round, std::mt19937 &random);

        // The match starts: who the captain is (the first crew member; they always return, rules section 2).
        void OnMatchStart();

        // The fight begins: the crew as they are (with Permanent Death off, everyone who dies in it returns).
        void OnFightStart();

        // The ship's whole crew is dead: no one alive (drones don't count), and no clone on the way.
        bool CrewGone(ShipManager *ship);

        // The round is over. Everyone alive goes home: our crew aboard the opponent's ship come back, theirs aboard
        // ours leave; crew aboard a destroyed ship die with it. Mind control ends.
        void EndOfRound(bool ownDestroyed, bool theirsDestroyed);

        // The preparation starts: hull, systems, rooms and crew as good as new (fires out, breaches sealed, air
        // back, locks, ion and hacking gone); the dead captain returns, and with Permanent Death off every dead crew
        // member (and anyone the clone bay was about to bring back); the crew go to their stations.
        void Restore(bool permadeath);

        // This round's scrap (the first round's replaces what the ship started with).
        void GiveScrap(bool firstRound, int amount);

        // The shop with the round's stock, open for the preparation; closed (with the upgrade screen) when the ships
        // meet.
        void OpenShop(int round, const std::vector<ShopItem> &stock);
        void CloseShop();

        // The fight begins as at a new beacon: weapons start uncharged.
        void ResetWeaponCharge();
    }
}
