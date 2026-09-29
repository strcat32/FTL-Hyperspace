#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
    // to that game, and our teleporter takes them back with whatever health they have there.
    namespace Boarding
    {
        static const uint8_t MSG_BOARD = 32;      // reliable, attacker -> defender: our crew arrived aboard your ship
        static const uint8_t MSG_RECALL = 33;     // reliable, attacker -> defender: our teleporter takes these back
        static const uint8_t MSG_RETURNED = 34;   // reliable, defender -> attacker: how they left (health, skills, or dead)

        void Reset();
        void OnFrame();
        void OnMessage(uint8_t type, Reader &r);

        // ShipManager::AddCrewMember, after it ran: our crew arriving on the replica (sent there by our teleporter)
        // or back home.
        void OnCrewArrived(ShipManager *ship, CrewMember *crew, int room);
        // CompleteShip::InitiateTeleport, after it ran (Hyperspace's rewrite picks the crew): our teleporter taking
        // our crew back from the replica goes to its owner.
        void AfterTeleport(CompleteShip *ship, int command);

        // ShipSystem::PartialDamage: the replica's systems are damaged by its owner's game (our crew aboard are only
        // puppets here; the owner's state brings the damage).
        bool MayDamage(const ShipSystem *system);

        // CrewAI::OnLoop runs (FTL's crew AI: it walks intruders from system to system, for example).
        void SetAiRunning(bool running);
        // ShipManager::CommandCrewMoveRoom: the crew AI doesn't move guests or puppets. Guests go where their owner
        // orders them (and fight or sabotage where they are, as FTL's boarders do); puppets follow their owners.
        bool RefusesAiOrder(const CrewMember *crew);

        // Test verb: teleport send <room> | teleport recall <room> (our teleporter, the opponent's room) | teleport
        // order <room> (our crew aboard go there).
        bool RunVerb(const std::string &what, std::string &message);

        std::string Status();
    }
}
