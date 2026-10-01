#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ProjectileFactory;
struct ShipManager;
struct WeaponBlueprint;

// Fair play (roadmap 4.1; docs/design/netcode-authority.md in the FTL: Duels repository, layer 1): dodge rolls that
// neither player can choose or foresee. Each game makes two hash chains for a match (a chain is a random value hashed
// again and again; its values are shown from the last hashed back, so each is checked against the one before it with
// one hash, and none can be worked out from those shown) and sends their ends when the duel connects. The attacker's
// i-th shot carries the i-th value of its shot chain, the defender's verdict on it the i-th value of its verdict chain.
// The roll: dodged when sha256(both values) mod 100 is under the evasion at impact. The attacker doesn't know the
// defender's value when it fires, and the defender can't pick its own (its chain is fixed), so neither steers a roll.
// The attacker checks every roll that comes back, and the evasion the verdict names against the defender's ship as its
// copy shows it; a mismatch is a dispute: logged and counted (the server referee takes them with 4.2).
namespace Duels
{
    class Reader;
    class Writer;
    struct Command;

    namespace Fair
    {
        static const uint8_t MSG_CHAINS = 41;   // reliable, either way: our chains' ends
        static const size_t VALUE_SIZE = 32;

        struct Value
        {
            uint32_t index = 0;   // 1-based; 0: none (no chains yet, or the chain is used up)
            uint8_t bytes[VALUE_SIZE] = {};
        };

        // The duel connects: a new match gets new chains and sends their ends; after a lost connection the same chains
        // go on and their ends go again.
        void OnConnected(bool resumed);
        void OnMessage(Reader &r);   // MSG_CHAINS

        // The attacker: our shot chain's next value, written into a shot going out (and kept with it).
        Value NextShot();
        void WriteValue(Writer &w, const Value &value);
        bool ReadValue(Reader &r, Value &value);

        // The defender: the roll for the opponent's shot, against the evasion at impact (as the verdict names it). False
        // when there is nothing to roll with (FTL rolls then, and the verdict says so); else `dodged`, and `verdict` is
        // our verdict chain's value for the shot, which goes back with the verdict.
        bool Roll(const Value &shot, int &evasion, Value &verdict, bool &dodged);

        // The attacker checks the defender's roll on its shot: the verdict's chain value, the outcome (dodged or not)
        // and the evasion named in it against the one our copy of their ship gives (-1: none to compare).
        void CheckRoll(const Value &shot, const Value &verdict, int evasion, bool dodged, int expectedEvasion, const std::string &what);

        // Layer 2 (consistency checks): the opponent's state, each time a newer one comes. In a fight its hull never
        // rises; its shield layers are never more than half its shield power; its systems never draw more than its
        // reactor and its battery give; its weapons charge no faster than FTL charges them (with re-loaders and a manned
        // weapons system; as a share of the full charge, which manning shortens and FTL rescales the charge with). A
        // breach that a moment's lag can't explain is a dispute.
        struct StateCheck
        {
            double sentAt = 0.0;                 // on their clock, ms
            bool fight = false;                  // the fight has begun, and the round isn't decided
            int hull = 0;
            int shieldLayers = -1, shieldPower = 0;   // -1: no shields
            int powerUsed = 0, powerAvailable = -1;   // the systems that draw on the reactor; -1: not known
            std::vector<float> charges, cooldowns;    // each weapon's charge and its full charge, as its owner has them
            bool hullRepair = false;                  // a hull repair drone of theirs is out (FTL's mends the hull)
        };
        void CheckState(const StateCheck &state);

        // Layer 2 from what is always seen (roadmap 4.5: their power and their weapons' charge may be hidden from us):
        // their shots. A weapon fires again no sooner than FTL can charge it: its blueprint's full charge (less a chain's
        // boosts), shortened by the best gunner (20%), at 1 + their re-loaders a second. And a weapon that fired was
        // powered while it charged: that with the power we do see drawn (shields, engines, cloak, hacking, mind control,
        // drones out) more than their reactor, their battery and their Zoltans give is a dispute.
        // firedMs: when it left their weapon, by their stamp on our clock (no later than it came, a lost message sent
        // again comes late: its volley stays one).
        void OnShot(int slot, const WeaponBlueprint *blueprint, ShipManager *replica, double firedMs);
        // Each state of theirs while we don't see all their power (CheckState checks it all when we do), when it was
        // sent on our clock: the power we see drawn on their reactor, and what the reactor, the battery and their Zoltans
        // give at most.
        void PowerSeen(double sentMs, int seenPower, int available, const std::string &what);

        // Test cheats on our own state, as SendState writes it ("fair cheat hull|shields|power|charge").
        int CheatHull(int hull);
        int CheatShieldLayers(int layers);
        int CheatPower(int power);
        float CheatCharge(float charge, float cooldown);
        // "fair cheat rapid": before ProjectileFactory::Update, our weapons charge twice as fast (really: more shots).
        void CheatCharging(ProjectileFactory *weapon);

        // Layer 5: the game's data that decides a fight, hashed (every weapon's, drone's and augment's numbers, every
        // player ship's systems, arms, hull and reactor); the handshake compares it (DuelsNet.cpp). "" until FTL has
        // loaded its data.
        std::string GameDataHash();

        int Disputes();
        std::string Status();

        // Test verb (debug mode): "fair" (the state), "fair cheat dodge|evasion <n>|chain|hull|shields|power|charge|rapid|off":
        // this game lies in its verdicts or its state, so that the other's checks can be seen to catch it.
        bool RunVerb(const Command &cmd, std::string &message);
    }
}
