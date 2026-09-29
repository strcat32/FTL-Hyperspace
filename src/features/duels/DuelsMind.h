#pragma once

#include <cstdint>
#include <string>

struct CrewMember;
struct MindSystem;
struct ShipManager;

namespace Duels
{
    class Reader;
    class Writer;

    // Mind control in a network duel (roadmap 2.5, docs/design/hacking.md). FTL's (Hyperspace's) mind control takes
    // up to its level's number of crew in a room for 14, 20 or 28 s; they fight for the controlling ship, and the
    // controlling player can give them orders; then the system is locked.
    //
    // The attacker's game runs its own mind control system and picks the crew (on the replica: puppets). It tells
    // the defender which crew (MSG_MIND, by the rosters' ids); the defender's game puts them under the replica's mind
    // control, whose timer follows the owner's (the state), and releases them with it. The puppets show their owner's
    // mind control (DuelsCrew.cpp), and the attacker's orders to them go to the defender's game (MSG_CREW_ORDER).
    namespace Mind
    {
        static const uint8_t MSG_MIND = 30;         // reliable, attacker -> defender: your crew with these ids are under my mind control
        static const uint8_t MSG_CREW_ORDER = 31;   // reliable, controller -> owner: move your crew member under my control

        void Reset();

        void OnMessage(uint8_t type, Reader &r);

        // The state message: our control's timer; the replica's follows it (only for the control it knows).
        void WriteState(Writer &w);
        bool ReadState(Reader &r);
        void ApplyState();

        // MindSystem::InitiateMindControl, after it ran: the opponent's crew our mind control took go to their owner.
        void AfterInitiate(MindSystem *system, size_t controlledBefore);

        // ShipManager::CommandCrewMoveRoom: an order to a puppet under our mind control, or to our crew aboard their
        // ship (DuelsBoarding.cpp), goes to the game that decides about them (true = handled here).
        bool OrderToOwner(ShipManager *ship, CrewMember *crew, int room);

        // Test verb: mind room <room> (our mind control on the enemy's crew in that room), mind order <room> (the crew
        // it holds go there).
        bool RunVerb(const std::string &what, std::string &message);

        // duels_sync.csv: the ids of the crew a ship's mind control holds ("idle" without).
        std::string Signature(ShipManager *ship);
        std::string Status();
    }
}
