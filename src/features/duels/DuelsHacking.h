#pragma once

#include <cstdint>
#include <string>

struct HackingSystem;
struct ShipManager;

namespace Duels
{
    class Reader;
    class Writer;

    // Hacking in a network duel (roadmap 2.5). FTL's hacking: the hacking system sends a drone to a system of the
    // enemy ship; while it flies, shots can destroy it (defense drones shoot at it); once it has attached, the system is
    // hacked, and the hacking button's pulse (4, 7 or 10 s by power) gives it FTL's hacked effect (shields drain,
    // weapons and drones lose power, doors lock, ...); then the hacking system is locked for 4 periods.
    //
    // Split authority, as with everything else: the attacker's game owns its hacking system (the launch, the pulse
    // button, the lock), the defender's game owns its ship and its space. In the defender's game the replica's hacking
    // system does the hacking, with FTL's own code, as an enemy ship hacks the player:
    //  - Launch: the attacker says which system its drone went for (MSG_HACK); the replica's drone leaves for ours.
    //  - Flight: our game decides what becomes of it in our space (our defense drones, as against any drone), and says
    //    so (MSG_HACK_RESULT: attached or destroyed). The attacker's pulse waits for "attached"; "destroyed" blows up
    //    the attacker's drone.
    //  - Pulse: the attacker's hacking system is locked while it pulses; the replica's lock and pulse timer follow
    //    (the state), so the replica's hacking system gives our system FTL's hacked effect for as long.
    //  - Stop: the attacker's hacking ends (no power, drone gone): the replica's stops too.
    // Weapon and drone bays: a drone at W1 or D1 hacks bay 1 (FTL's lookup of the system in a room gives the bay).
    namespace Hacking
    {
        static const uint8_t MSG_HACK = 28;          // reliable, attacker -> defender: our drone left for a system of yours, or our hacking stopped
        static const uint8_t MSG_HACK_RESULT = 29;   // reliable, defender -> attacker: your drone attached, or was destroyed in my space

        void Reset();

        // Every frame in a duel: our launches and stops go to the opponent; what became of the opponent's drone here
        // goes back to them.
        void OnFrame();
        void OnMessage(uint8_t type, Reader &r);

        // The state message: our pulse's progress; the replica's follows it (after the locks are applied).
        void WriteState(Writer &w);
        bool ReadState(Reader &r);
        void ApplyState();

        // HackingSystem::InitiatePulse: our pulse waits for the defender's word that the drone attached.
        bool MayPulse(HackingSystem *system);

        // Test verb: hack <system> (our drone goes for the opponent's system), hack pulse, hack stop.
        bool RunVerb(const std::string &what, std::string &message);

        // duels_sync.csv: the system the hacking drone went for, "a" once attached, "p" while pulsing.
        std::string Signature(ShipManager *ship);
        std::string Status();
    }
}
