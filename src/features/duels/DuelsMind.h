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
    // The attacker's game runs its own mind control system; aimed at the defender's ship, the room goes to the
    // defender (MSG_MIND), whose game picks the crew there by FTL's rules (roadmap 4.5: the attacker's copy of that
    // ship knows where its crew are only where it sees them), puts them under the replica's mind control, whose timer
    // follows the owner's (the state), and releases them with it. It answers with the crew it took (MSG_MIND_TAKEN, by
    // the rosters' ids): the attacker's control holds those puppets from then on. The puppets show their owner's mind
    // control (DuelsCrew.cpp), and the attacker's orders to them go to the defender's game (MSG_CREW_ORDER).
    namespace Mind
    {
        static const uint8_t MSG_MIND = 30;         // reliable, attacker -> defender: my mind control on your room (pick whom it takes)
        static const uint8_t MSG_CREW_ORDER = 31;   // reliable, controller -> owner: move your crew member under my control
        static const uint8_t MSG_MIND_TAKEN = 42;   // reliable, defender -> attacker: whom your mind control took (and freed, and who resisted)

        void Reset();

        void OnMessage(uint8_t type, Reader &r);

        // The state message: our control's timer; the replica's follows it (only for the control it knows).
        void WriteState(Writer &w);
        bool ReadState(Reader &r);
        void ApplyState();

        // MindSystem::QueueMindControl: our mind control aimed at the opponent's ship goes to their game (true =
        // handled here; FTL's queue stays empty).
        bool QueueToOwner(MindSystem *system, int room, int shipId);

        // The player whose mind control took an enemy crew member gives it orders (rules, section 2): Hyperspace allows
        // that only with the MIND_ORDER augment, which our ship has in a duel (ShipObject::HasAugmentation; roadmap CS:
        // the crew taken couldn't be selected, so no order went).
        bool OrdersFromUs();

        // ShipManager::CommandCrewMoveRoom: an order to a puppet under our mind control, or to our crew aboard their
        // ship (DuelsBoarding.cpp), goes to the game that decides about them (true = handled here).
        bool OrderToOwner(ShipManager *ship, CrewMember *crew, int room);

        // Test verb: mind room <room> (our mind control on the enemy's crew in that room), mind ui <room> (the same as
        // a player does it: FTL's button arms it, a click on the room aims it), mind own <room> (on the opponent's crew
        // aboard our ship in that room, roadmap 3.8), mind order <room> (the crew it holds go there).
        bool RunVerb(const std::string &what, std::string &message);

        // CrewMember::UpdateRepair: a crew member of the opponent's that our mind control holds aboard our ship repairs
        // our systems as ours do (roadmap 3.8); FTL has a crew member of another ship sabotage there.
        bool RepairsForUs(const CrewMember *crew);

        // duels_sync.csv: the ids of the crew a ship's mind control holds ("idle" without): on our ship the opponent's
        // crew (puppets on theirs, guests on ours, by their ids); on the replica ours (aboard either ship, by our ids).
        std::string Signature(ShipManager *ship);
        std::string Status();
    }
}
