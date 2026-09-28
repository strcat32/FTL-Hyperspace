#pragma once

#include <cstdint>
#include <string>

struct Collideable;
struct Drone;
struct Projectile;
struct ShipManager;
struct SpaceDrone;

namespace Duels
{
    class Reader;
    class Writer;

    // Drones in a network duel. Each game simulates its own drones; the opponent's drones in our game are puppets (the
    // replica's drones): powered, launched and destroyed as their owner's are, moved to where the owner's are, and never
    // firing or acting on their own. What happens in a space is decided by the game whose space it is:
    //  - Our space (0): our ship, our defense, repair and shield drones, the opponent's combat drones, their shots at us.
    //    Our defense drones shoot their shots down here (the verdict "downed"), and we report hits on their drones here.
    //  - Their space (1): nothing collides but our shots with their ship, which wait for the defender's verdict there.
    // Combat drone shots travel like weapon shots (DuelsMatch.cpp); defense drone shots are shown to the opponent as
    // harmless copies.
    namespace Drones
    {
        static const uint8_t MSG_DRONE_HIT = 24;    // reliable: a drone of yours was hit in my space
        static const uint8_t MSG_DRONE_SHOT = 25;   // reliable: one of my drones fired in my space (a copy for your screen)

        void Reset();

        // The state message: our drones; the opponent's are read with it and applied to the replica's after its systems
        // (localTime: when the owner sent it, on our clock).
        void WriteState(Writer &w);
        bool ReadState(Reader &r);
        void ApplyState(double localTime);
        std::string Signature(ShipManager *ship);   // duels_sync.csv: drone parts, and each slot's power, launch, wreck
        std::string Status();                       // for "net"

        void OnMessage(uint8_t type, Reader &r);

        // After SpaceManager::OnLoop: the puppets take their owners' positions (rendering comes after).
        void AfterSpaceLoop();

        // A drone of the replica, driven by the network.
        bool IsPuppet(const Drone *drone);
        // The drone's slot in its ship's drone system, or -1.
        int SlotOf(ShipManager *ship, const Drone *drone);

        // --- hook entry points ---

        // SpaceDrone::GetNextProjectile: false = a puppet, which must not fire.
        bool MayFire(SpaceDrone *drone);
        // A drone of ours fired in our space (defense drones): the opponent gets a harmless copy to see.
        void OnOwnDroneShotInOwnSpace(SpaceDrone *drone, Projectile *projectile);

        // Harmless copies of the opponent's drone shots: they never collide.
        bool IsVisualShot(const Projectile *projectile);
        // Our drones' shots in our space: the only projectiles that may hit other projectiles.
        bool IsOwnDroneShot(const Projectile *projectile);

        // A projectile ran into a drone (SpaceDrone::CollisionMoving): report hits on the opponent's drones in our space.
        void ObserveDroneCollision(SpaceDrone *drone, bool explodingBefore, float ionStunBefore);

        // Drone::OnLoop: a stunned puppet must not roll its own chance to explode.
        bool RunsOwnLoop(Drone *drone);

        // Replica effects that belong to the owner: a repair drone's +1 hull, a shield drone's super shield.
        bool BlocksReplicaRepair(ShipManager *ship, int damage);

        // A "downed" verdict exploded one of our shots near here: harmless copies aimed there go with it.
        void EndVisualShotsNear(float x, float y, int space);
    }
}
