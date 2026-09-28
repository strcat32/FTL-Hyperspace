#pragma once

#include <cstdint>
#include <string>

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
        void WriteState(Writer &w);

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
        std::string Status();

        // --- hook entry points ---

        // A crew member of the replica (a puppet): its health follows its owner, nothing else changes it.
        bool IsPuppet(const CrewMember *crew);
        // ShipSystem::PartialRepair: the replica's systems are repaired by their owner (the state brings the health).
        bool MayRepair(const ShipSystem *system);
    }
}
