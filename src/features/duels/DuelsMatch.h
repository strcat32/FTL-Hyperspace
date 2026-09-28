#pragma once

#include <cstdint>
#include <string>

struct BombProjectile;
struct Collideable;
struct CollisionResponse;
struct Damage;
struct Pointf;
struct Projectile;
struct ProjectileFactory;
struct ShipManager;
struct SpaceDrone;

// A duel between two players over the network (Step 2), under split authority: each game owns its own ship
// (ship 0). The opponent is ship 1, a replica driven by the owner's messages.
//
//  - On connecting, both send their loadout; each spawns the other's ship as ship 1 and applies it.
//  - Ten times a second each sends its ship's state (hull, shields, systems, weapons); the replica follows it.
//  - A shot is captured when the owner's weapon releases a projectile (laser, missile, flak shard, beam or bomb) and
//    sent with its exact target point. The defender creates it on its side and lets its own game decide the hit
//    (dodge, shields, damage). The verdict goes back; until it arrives, the attacker's copy of the shot waits at the
//    edge of the target's shield (a bomb: where it goes off). A beam's damage is only done by the defender's game.
//
// Hooks live in DuelsHooks.cpp and call in here.
namespace Duels
{
    namespace Match
    {
        void Init();
        void OnFrame(double now);

        void SetPlayerName(const std::string &name);
        bool Host(uint16_t port, bool loopbackOnly, std::string &message);
        bool Join(const std::string &host, uint16_t port, std::string &message);
        void Leave();
        bool Say(const std::string &text);
        std::string Status();

        // --- hook entry points ---

        // ProjectileFactory::GetProjectile released a projectile of one of our weapons.
        void OnOwnProjectile(ProjectileFactory *weapon, Projectile *projectile);

        // SpaceDrone::GetNextProjectile released a projectile of one of our drones: a combat drone's shot at the replica
        // goes like a weapon's; a defense drone's shot in our space is shown to the opponent (DuelsDrones.cpp).
        void OnOwnDroneProjectile(SpaceDrone *drone, Projectile *projectile);

        // SpaceManager::UpdateProjectile: returns how many times to run the update this frame (0 = hold, 1 = normal,
        // more = catch up).
        int ProjectileUpdates(Projectile *projectile);

        // Projectile::CollisionCheck: false = skip this check. In a duel, what may collide follows whose space it is
        // (DuelsDrones.h), and our shot at the replica waits for the verdict. Sets up the forced outcome for the calls
        // below when a verdict exists.
        bool BeginCollisionCheck(Projectile *projectile, Collideable *other);
        void EndCollisionCheck();

        // The forced outcome of our shot at the replica, if one applies to this ship and projectile.
        bool ForcedShieldResponse(ShipManager *ship, Pointf start, Pointf finish, const Damage &damage,
                                  CollisionResponse &response);
        bool ForcedDamageArea(ShipManager *ship, Pointf location, bool &hit);

        // The defender's own game decided an incoming shot (after the original functions ran).
        void ObserveShield(ShipManager *ship, const CollisionResponse &response);
        void ObserveDamageArea(ShipManager *ship, bool hit, int hullBefore);

        // Bombs (BombProjectile::CollisionCheck, ShipManager::GetDodged): ours in the replica goes off, or misses,
        // as the defender's did, and waits for that verdict; the defender's dodge is reported when it is rolled.
        bool BeginBombCheck(BombProjectile *bomb, Collideable *other);
        bool ForcedDodge(ShipManager *ship, bool &dodged);
        void ObserveDodge(ShipManager *ship, bool dodged);

        // Beams (ShipManager::DamageBeam): ours sweeps the replica without damage; the defender's is reported when over.
        void MuteBeamDamage(ShipManager *ship, Damage &damage);
        void ObserveBeam(ShipManager *ship, bool hit, int hullBefore);
    }
}
