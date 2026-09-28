#pragma once

#include <cstdint>
#include <string>

struct ShipManager;

namespace Duels
{
    class Reader;
    class Writer;

    // The rooms of each ship in a network duel (roadmap 2.2): oxygen, fires, hull breaches, doors and crystal
    // lockdowns. The owner's game decides them on its own ship, as everything else there. The replica shows its
    // owner's: its own environment (fire spreading, oxygen, breaches letting air out) doesn't run, and what its crew
    // (puppets) do to fires and breaches is undone every frame.
    namespace Rooms
    {
        void Reset();

        // The state message: our rooms; the opponent's are read with it and applied to the replica.
        void WriteState(Writer &w);
        bool ReadState(Reader &r);
        void ApplyState();

        // After the replica's ShipManager::OnLoop: oxygen, fires and breaches are held at their owner's values.
        void AfterReplicaLoop(ShipManager *replica);

        // duels_sync.csv: oxygen per room (in steps of 10), and burning tiles, breaches, open doors, lockdowns.
        std::string Signature(ShipManager *ship);
        std::string Status();

        // ShipManager::UpdateEnvironment: false for the replica, whose rooms follow their owner.
        bool RunsEnvironment(ShipManager *ship);
    }
}
