#pragma once

#include <cstdint>
#include <string>

struct Collideable;
struct CollisionResponse;
struct Damage;
struct Pointf;
struct Projectile;
struct ProjectileFactory;
struct ShipManager;

// A duel between two players over the network (Step 2), under split authority: each game owns its own ship
// (ship 0). The opponent is ship 1, a replica driven by the owner's messages.
//
//  - On connecting, both send their loadout; each spawns the other's ship as ship 1 and applies it.
//  - Ten times a second each sends its ship's state (hull, shields, systems, weapons); the replica follows it.
//  - A shot is captured when the owner's weapon releases a projectile and sent with its exact target point. The
//    defender creates it on its side and lets its own game decide the hit (dodge, shields, damage). The verdict
//    goes back; until it arrives, the attacker's copy of the shot waits at the edge of the target's shield.
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

        // SpaceManager::UpdateProjectile: returns how many times to run the update this frame (0 = hold, 1 = normal,
        // more = catch up).
        int ProjectileUpdates(Projectile *projectile);

        // Projectile::CollisionCheck against a ship: false = skip this check (our shot waits for the verdict).
        // Sets up the forced outcome for the calls below when a verdict exists.
        bool BeginCollisionCheck(Projectile *projectile, Collideable *other);
        void EndCollisionCheck();

        // The forced outcome of our shot at the replica, if one applies to this ship and projectile.
        bool ForcedShieldResponse(ShipManager *ship, Pointf start, Pointf finish, const Damage &damage,
                                  CollisionResponse &response);
        bool ForcedDamageArea(ShipManager *ship, Pointf location, bool &hit);

        // The defender's own game decided an incoming shot (after the original functions ran).
        void ObserveShield(ShipManager *ship, const CollisionResponse &response);
        void ObserveDamageArea(ShipManager *ship, bool hit, int hullBefore);
    }
}
