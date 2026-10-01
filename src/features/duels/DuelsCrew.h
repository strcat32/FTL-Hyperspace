#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct CrewAnimation;
struct CrewMember;
struct ShipManager;
struct ShipSystem;

namespace Duels
{
    class Reader;
    class Writer;

    // Crew in a network duel (docs/design/crew-sync.md). Each game decides everything about its own crew on its own
    // ship. The opponent's crew in our game are puppets: the replica's crew, made from the owner's roster, walking
    // where the owner's walk (with FTL's own pathfinding), showing their health and deaths, and never changing the
    // replica themselves (no repairs, no health changes of their own).
    namespace Crew
    {
        static const uint8_t MSG_CREW_ROSTER = 26;   // reliable: who is on board (ids, species, names, skills)

        void Reset();

        // Our side. The roster goes out when it changed (checked with every state); the state message carries where
        // each crew member is and how they are.
        bool RosterChanged();
        void WriteRoster(Writer &w);
        // After a lost connection: the roster goes again with the next state.
        void SendRosterAgain();
        // record: our ship's full state for a demo, written right after the one that goes (DuelsDemo.cpp): the same
        // ids, without the ids' bookkeeping, and with the guests that state reported dead.
        void WriteState(Writer &w, bool record = false);

        // Their side: the roster makes the puppets; the state moves them (localTime: when the owner sent it, on our
        // clock).
        void ApplyRoster(Reader &r);
        bool ReadState(Reader &r);
        void ApplyState(double localTime);

        // After the replica's ShipManager::OnLoop: puppets keep their owners' health, and are put back in place when
        // they walked too far off.
        void AfterReplicaLoop(ShipManager *replica);

        // duels_sync.csv: each crew member's id, health and whether dead; and, separately, the room each one is in
        // (crew walking the same distance by another route are in other rooms for a moment).
        std::string Signature(ShipManager *ship);
        std::string RoomSignature(ShipManager *ship);
        // And the animation each one shows (FTL's animation number; "t" when typing at a console).
        std::string AnimationSignature(ShipManager *ship);
        std::string Status();

        // --- hook entry points ---

        // A crew member of the replica (a puppet): its health follows its owner, nothing else changes it.
        bool IsPuppet(const CrewMember *crew);
        // A puppet the owner's latest state left out (roadmap 4.5: we can't see where they are): not drawn.
        bool IsHidden(const CrewMember *crew);
        // A crew member's health as the messages carry it: whole points, and at least 1 while alive (FTL's crew live
        // on at 0.4; a copy given 0 would die).
        int WireHealth(const CrewMember *crew);

        // Crew by the ids the rosters give them (the owner's ids, for ours and the puppets alike): a puppet's id, or
        // -1; our crew member with an id, or null.
        int PuppetId(const CrewMember *crew);
        CrewMember *PuppetById(uint16_t id);
        int OwnId(const CrewMember *crew);
        CrewMember *OwnById(uint16_t id);

        // Boarding (DuelsBoarding.cpp, docs/design/boarding.md): crew aboard the other ship are decided by the game
        // whose ship it is.
        // Ours arriving on the replica: they become puppets of the owner's guest state there (by our id, returned;
        // -1 if the crew member has none); back home, ours again with the same id.
        int BoardAway(CrewMember *crew);
        void CameHome(uint16_t id);
        int AwayId(const CrewMember *crew);
        // Boarding drones: an id for a robot of ours (from our crew ids, so the two never meet); our robot on the
        // replica is the puppet of the defender's guest entry for it.
        uint16_t NewRobotId();
        void RobotAway(CrewMember *robot, uint16_t id);
        // Theirs aboard our ship (by their owner's ids): ours to simulate, in our crew state as guests.
        void AddGuest(uint16_t id, CrewMember *crew);
        CrewMember *Guest(uint16_t id);
        bool IsGuest(const CrewMember *crew);
        int GuestIdOf(const CrewMember *crew);   // its owner's id, -1 if it is no guest
        void RemoveGuest(uint16_t id);
        // A guest taken back by their teleporter, now on the replica: the puppet for their id (the next roster has
        // them again).
        void AdoptPuppet(uint16_t id, CrewMember *crew);
        // The end of a round (DuelsRefit.cpp): ours aboard the opponent's ship (with their ids), and theirs aboard ours.
        std::vector<std::pair<uint16_t, CrewMember*>> AwayCrew();
        std::vector<std::pair<uint16_t, CrewMember*>> Guests();
        // ShipSystem::PartialRepair: the replica's systems are repaired by their owner (the state brings the health).
        bool MayRepair(const ShipSystem *system);
        // CrewAnimation::OnUpdate: whether a crew member is shown fighting, repairing (a system, a breach or a fire),
        // standing in a fire and typing at a console. Ours is noted for the state; a puppet shows its owner's.
        void Animate(CrewAnimation *anim, bool &fighting, bool &repairing, bool &onFire);
    }
}
