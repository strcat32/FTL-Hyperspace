#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

struct ReactorButton;
struct ShipManager;
struct UpgradeBox;

namespace Duels
{
    class Reader;
    class Writer;

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

        // The host: a round's stock. A page for each kind, every round (roadmap AP): weapons, drones, augments,
        // systems and crew, six of each, drawn as FTL's stores draw them (weight 6 - rarity) within the round's price
        // cap for weapons and drones; missiles and drone parts.
        std::vector<ShopItem> MakeStock(int round, std::mt19937 &random);

        // The match starts: who the captain is (the first crew member; they always return, rules section 2).
        void OnMatchStart();
        // Back after a crash (roadmap BR, DuelsRejoin.cpp): what the refit keeps over the match (the levels as it began,
        // the captain, the crew and the power as the last fight began) for the file, and from it in place of
        // OnMatchStart when the game is back in the match.
        void WriteRejoin(Writer &w);
        void ColdStart(const std::vector<uint8_t> &saved);

        // The fight begins: the crew as they are (with Permanent Death off, everyone who dies in it returns).
        void OnFightStart();

        // The ship's whole crew is dead: no one alive (drones don't count), and no clone on the way. (Ours aboard the
        // other ship count: in a duel the crew registry knows them, in a match against the AI FTL's own ship ids.)
        bool CrewGone(ShipManager *ship);

        // The round is over. Everyone alive goes home: our crew aboard the opponent's ship come back, theirs aboard
        // ours leave; crew aboard a destroyed ship die with it. Mind control ends.
        void EndOfRound(bool ownDestroyed, bool theirsDestroyed);

        // The preparation starts: hull, systems, rooms and crew as good as new (fires out, breaches sealed, air
        // back, locks, ion and hacking gone); the dead captain returns, and with Permanent Death off every dead crew
        // member (and anyone the clone bay was about to bring back); the crew go to their stations.
        void Restore(bool permadeath);

        // Between fights (Rounds::BetweenFights: the ship choice, the preparation, the ships meeting, a round's or the
        // match's end) the air stays full in every room, as Restore left it. Without an oxygen system the rooms have
        // none, as in FTL (the crew is kept alive by Rounds::BetweenFights until the fight).
        void KeepAir();

        // This round's scrap (the first round's replaces what the ship started with).
        void GiveScrap(bool firstRound, int amount);

        // The shop with the round's stock, open for the preparation; closed (with the upgrade screen) when the ships
        // meet.
        void OpenShop(int round, const std::vector<ShopItem> &stock);
        void CloseShop();
        // The store shows the description of the item under the mouse, right of it (roadmap BY: the preparation's
        // countdown and READY covered it; they step aside meanwhile).
        bool StoreDescriptionShown();
        // The ship's screens (upgrades, crew, cargo) 20 px lower than FTL has them (roadmap CW: the score panel covered
        // their tabs): in the preparation, and in a replay (DN).
        void PlaceShipScreens();
        // Each frame of a preparation: after a system's sale the store is built again (once its window is closed), with
        // the system on the buy-back page (roadmap BJ: a sixth page, our sold systems at FTL's price).
        void OnPrepFrame();

        // The fight begins as at a new beacon: weapons and artillery start uncharged, the weapons full with a Weapon
        // Pre-Igniter (FTL fills them as a ship arrives; roadmap CO).
        void ResetWeaponCharge();

        // Each frame of a preparation and the ships' meeting: our weapons and artillery stay empty (FTL charges powered
        // ones any time, and fires an artillery as soon as it is charged and there is a ship to aim at; roadmap CO).
        void HoldCharges();

        // The store and the ship's screens (upgrades, crew, equipment) in the preparation (roadmap Q). FTL sends every
        // click and key to its open window, so with the store open (it opens by itself) the upgrade button and the
        // U, C and I keys did nothing, and with the upgrade screen open the STORE button did nothing. In a match's
        // preparation they switch: the open window closes (upgrades already paid for are made) and the other opens.
        // SwitchScreensClick takes the click when it switched; SwitchScreensKey only closes the store (or the ship's
        // screens), and FTL then opens what the key asks for.
        bool SwitchScreensClick(int x, int y);
        void SwitchScreensKey(int key);

        // The shop buys back (roadmap V; rules, section 7), in a match's preparation, in FTL's upgrade screen. A
        // right-click on a system or the reactor with no upgrade waiting (FTL's own right-click takes one of those
        // back) takes a level back for all it cost (AJ), down to level 1. At level 1 a system (any of FTL's but the
        // reactor, AK) is sold at a second right-click within 3 s, for all that it and its levels cost; its weapons or
        // drones go to the cargo (Hyperspace's RemoveSystem). The reactor goes down to 1 bar. True when
        // the right-click was taken. RenderSaleMark marks a box whose sale waits for the second click; OnUpgradesOpen
        // shows the tip once a match. The sale itself happens in OnUpgradesLoop (Upgrades::OnLoop, before FTL's own):
        // it builds the screen's boxes anew, which a right-click can't do while FTL goes through them.
        bool TakeBackLevel(UpgradeBox *box, int mouseX, int mouseY);
        bool TakeBackReactor(ReactorButton *button);
        void RenderSaleMark(UpgradeBox *box);
        // Where the upgrade screen last drew a system's box (its button's hit box), for the test verb upgradeclick:
        // Hyperspace keeps the boxes in pages of its own, not in FTL's list.
        bool BoxPlace(int systemId, int &x, int &y);
        void OnUpgradesOpen();
        void OnUpgradesLoop();

        // A ship's reactor (its blueprint's): the price of its bar `bar` (Hyperspace's reactor prices, as its
        // ReactorButton charges them), and the most bars it can have. For the AI's shopping too (DuelsAi.cpp).
        int ReactorPrice(const std::string &blueprint, int bar);
        int ReactorMax(const std::string &blueprint);
    }
}
