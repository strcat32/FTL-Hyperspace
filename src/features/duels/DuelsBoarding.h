#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ActivatedPower;
struct BoarderPodDrone;
struct CompleteShip;
struct CrewMember;
struct ShipManager;
struct ShipSystem;

namespace Duels
{
    class Reader;

    // Boarding in a network duel (roadmap 2.6, docs/design/boarding.md): what happens aboard a ship is decided by the
    // game whose ship it is. Our crew teleported to the opponent's ship become guests there (their game simulates
    // them: walking, fights, sabotage, health, death) and puppets here (DuelsCrew.cpp: "away"); our orders to them go
    // to that game, and our teleporter takes them back with whatever health they have there. A boarding drone's pod
    // lands where the defender's copy of it landed (the defender's space, the defender's pick), and its robot is a
    // guest there like teleported crew.
    namespace Boarding
    {
        static const uint8_t MSG_BOARD = 32;      // reliable, attacker -> defender: our crew arrived aboard your ship
        static const uint8_t MSG_RECALL = 33;     // reliable, attacker -> defender: our teleporter takes these back
        static const uint8_t MSG_RETURNED = 34;   // reliable, defender -> attacker: how they left (health, skills, or dead)
        static const uint8_t MSG_POD = 35;        // reliable, attacker -> defender: our boarding drone left (slot, robot id)
        static const uint8_t MSG_POD_RESULT = 36; // reliable, defender -> attacker: where its copy goes, lands, or shot down
        static const uint8_t MSG_CREW_POWER = 37; // reliable, owner -> the other game: our crew member aboard your ship used a power

        void Reset();
        void OnFrame();
        void OnMessage(uint8_t type, Reader &r);

        // ShipManager::AddCrewMember, after it ran: our crew arriving on the replica (sent there by our teleporter)
        // or back home.
        void OnCrewArrived(ShipManager *ship, CrewMember *crew, int room);
        // CompleteShip::InitiateTeleport, after it ran (Hyperspace's rewrite picks the crew): our teleporter taking
        // our crew back from the replica goes to its owner.
        void AfterTeleport(CompleteShip *ship, int command);
        // CompleteShip::InitiateTeleport, before: the replica never teleports on its own (FTL's enemy ship takes its
        // hurt boarders back by itself); its owner's teleporter does, and says so (MSG_RECALL).
        bool RefusesTeleport(const CompleteShip *ship);

        // ShipSystem::PartialDamage: the replica's systems are damaged by its owner's game (our crew aboard are only
        // puppets here; the owner's state brings the damage).
        bool MayDamage(const ShipSystem *system);

        // BoarderPodDrone::OnLoop, before it runs: false while our pod waits at its destination for the defender's
        // word (the defender's copy decides where it lands, or that it was shot down).
        bool MayPodLoop(BoarderPodDrone *pod);

        // CrewAI::OnLoop runs (FTL's crew AI: it walks intruders from system to system, for example).
        void SetAiRunning(bool running);
        // ShipManager::CommandCrewMoveRoom: the crew AI doesn't move guests or puppets. Guests go where their owner
        // orders them (and fight or sabotage where they are, as FTL's boarders do); puppets follow their owners. A
        // guest robot (a boarding drone's) takes no orders: the crew AI of the ship it is on moves it, as in FTL. A
        // guest our mind control holds is ours while it lasts (roadmap 3.8): our crew AI moves it too.
        bool RefusesAiOrder(const CrewMember *crew);

        // Hyperspace's crew powers (ActivatedPower; in vanilla FTL only the crystal crew's lockdown): a crew member's
        // owner decides when a power is used; the game whose ship it happens on applies it, and the state brings the
        // effect back. Two calls from Hyperspace's CrewAbilities.cpp:
        // ActivatedPower::OnUpdate: the opponent's crew in our game never use a power by themselves (puppets on their
        // ship; guests aboard ours, which Hyperspace would let use one in a fight, as FTL's enemy crew do).
        bool PowersHeld(const CrewMember *crew);
        // ActivatedPower::PreparePower: when one of ours aboard the replica uses a power, the opponent's game uses it
        // for its guest (MSG_CREW_POWER).
        void OnPowerPrepared(ActivatedPower *power);

        // Test verb: teleport send <room> | teleport recall <room> (our teleporter, the opponent's room) | teleport
        // order <room> (our crew aboard go there).
        bool RunVerb(const std::string &what, std::string &message);

        std::string Status();
    }
}
