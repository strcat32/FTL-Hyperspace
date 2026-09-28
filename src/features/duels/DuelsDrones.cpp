#include "Global.h"
#include "CommandConsole.h"
#include "Duels.h"
#include "DuelsConsole.h"
#include "DuelsDrones.h"
#include "DuelsNet.h"
#include "DuelsTrace.h"
#include "DuelsWire.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <set>
#include <sstream>
#include <vector>

namespace Duels
{
    namespace Drones
    {
        // Puppets are drawn behind their owner's updates by how old those are when they arrive (the latency), plus one
        // update interval and room for jitter, so there is always an update ahead to move towards. When one is lost,
        // they go on along their last movement for a while.
        static const double UPDATE_INTERVAL_MS = 100.0;
        static const double JITTER_ROOM_MS = 40.0;
        static const double MAX_EXTRAPOLATION_MS = 150.0;
        static const size_t MAX_SAMPLES = 16;

        // Per drone slot in the state message.
        enum : uint8_t
        {
            FLAG_POWERED = 1,
            FLAG_DEPLOYED = 2,
            FLAG_EXPLODING = 4,      // hit: its blast is playing
            FLAG_TARGET_SPACE = 8,   // flies in the opponent's space (combat drones)
            FLAG_POSITION = 16,      // a launched space drone: where it is follows
            FLAG_COMBAT = 32,        // combat-drone movement fields follow (combat and hull repair drones)
            FLAG_DEAD = 64           // destroyed, or used up without a blast (a hull repair drone after its last repair)
        };

        // DRONE_HIT kinds.
        enum : uint8_t
        {
            HIT_DESTROYED = 1,
            HIT_ION = 2
        };

        struct Sample
        {
            double t = 0.0;
            float x = 0.f, y = 0.f;
            float aimingAngle = 0.f;
            float pause = 0.f;
            float ionStun = 0.f;
            float weaponCooldown = 0.f;
            bool combat = false;
            float heading = 0.f, oldHeading = 0.f, progress = 0.f;
        };

        struct Entry
        {
            uint8_t flags = 0;
            float destroyedTimer = 0.f;
            Sample sample;
        };

        struct Puppet
        {
            uint8_t flags = 0;
            std::deque<Sample> samples;
            bool placed = false;           // has taken an owner's position since it was launched
            bool launchFailedLogged = false;
        };

        struct TrackedShot
        {
            Projectile *projectile = nullptr;
            unsigned int selfId = 0;
            Pointf target;
        };

        struct DroneState
        {
            std::vector<Puppet> puppets;           // by the replica's drone slot
            std::vector<Entry> pending;            // the last state message, applied after the replica's systems
            int pendingParts = 0;
            bool havePending = false;
            std::vector<TrackedShot> visual;       // harmless copies of the opponent's drone shots
            std::vector<TrackedShot> own;          // our drones' shots in our space
            std::set<std::string> notNetworked;    // drones announced as not networked yet
            uint32_t hitsSent = 0, hitsReceived = 0, shotCopies = 0;
            double updateAgeMs = -1.0;             // how old the owner's updates are when they arrive (smoothed)
            CsvFile trace;                         // duels_drones.csv with "trace on": where each puppet is drawn
        };

        static DroneState g_drones;

        void Reset()
        {
            g_drones.puppets.clear();
            g_drones.pending.clear();
            g_drones.havePending = false;
            g_drones.visual.clear();
            g_drones.own.clear();
            g_drones.notNetworked.clear();
            g_drones.updateAgeMs = -1.0;
        }

        // The replica's AI is replaced while it is a duel opponent (or a command-driven enemy).
        static bool Replaced()
        {
            return GetState().aiOff[1] && G_->GetShipManager(1) != nullptr;
        }

        // Space drones this module handles: defense, combat, hull repair and shield drones. (Boarding and hacking drones
        // come with crew, step 3.)
        static SpaceDrone *AsSpaceDrone(Drone *drone)
        {
            if (!drone) return nullptr;
            switch (drone->type)
            {
            case 0: case 1: case 5: case 7:
                return static_cast<SpaceDrone*>(drone);
            default:
                return nullptr;
            }
        }

        static bool CombatLike(const Drone *drone)
        {
            return drone->type == 1 || drone->type == 5;
        }

        bool IsPuppet(const Drone *drone)
        {
            return drone && drone->iShipId == 1 && Replaced();
        }

        int SlotOf(ShipManager *ship, const Drone *drone)
        {
            if (!ship || !ship->droneSystem) return -1;
            const std::vector<Drone*> &drones = ship->droneSystem->drones;
            for (size_t slot = 0; slot < drones.size(); ++slot)
            {
                if (drones[slot] == drone) return (int)slot;
            }
            return -1;
        }

        static SpaceDrone *DroneInSlot(ShipManager *ship, int slot)
        {
            if (!ship || !ship->droneSystem || slot < 0 || slot >= (int)ship->droneSystem->drones.size()) return nullptr;
            return AsSpaceDrone(ship->droneSystem->drones[slot]);
        }

        // --------------------------------------------------------------------------------------------------------
        // State
        // --------------------------------------------------------------------------------------------------------

        void WriteState(Writer &w)
        {
            ShipManager *ship = G_->GetShipManager(0);
            std::vector<Drone*> drones = ship && ship->droneSystem ? ship->droneSystem->drones : std::vector<Drone*>();
            w.I16((int16_t)(ship && ship->droneSystem ? ship->GetDroneCount() : 0));
            w.U8((uint8_t)std::min<size_t>(drones.size(), 255));
            for (Drone *drone : drones)
            {
                SpaceDrone *space = AsSpaceDrone(drone);
                // Boarding drones and the drones that work inside ships (anti-personnel, system repair) come with
                // crew, in step 3.
                if (!space && drone->deployed && drone->blueprint &&
                    g_drones.notNetworked.insert(drone->blueprint->name).second)
                {
                    Log("Drones: %s is not networked yet", drone->blueprint->name.c_str());
                    Console::Print("DUEL: " + drone->blueprint->name + " is not networked yet; the opponent won't see it");
                }
                uint8_t flags = 0;
                if (drone->powered) flags |= FLAG_POWERED;
                if (drone->deployed) flags |= FLAG_DEPLOYED;
                if (space)
                {
                    if (space->explosion.tracker.running) flags |= FLAG_EXPLODING;
                    if (drone->bDead) flags |= FLAG_DEAD;
                    if (space->currentSpace != drone->iShipId) flags |= FLAG_TARGET_SPACE;
                    if (drone->deployed && !drone->bDead && space->currentLocation.x > -1.0e30f) flags |= FLAG_POSITION;
                    if (CombatLike(drone)) flags |= FLAG_COMBAT;
                }
                w.U8(flags);
                w.F32(drone->destroyedTimer);
                if (flags & FLAG_POSITION)
                {
                    w.F32(space->currentLocation.x);
                    w.F32(space->currentLocation.y);
                    w.F32(space->aimingAngle);
                    w.F32(space->pause);
                    w.F32(space->ionStun);
                    w.F32(space->weaponCooldown);
                    if (flags & FLAG_COMBAT)
                    {
                        CombatDrone *combat = static_cast<CombatDrone*>(space);
                        w.F32(combat->heading);
                        w.F32(combat->oldHeading);
                        w.F32(combat->progressToDestination);
                    }
                }
            }
        }

        bool ReadState(Reader &r)
        {
            g_drones.pendingParts = r.I16();
            std::vector<Entry> entries(r.U8());
            for (Entry &entry : entries)
            {
                entry.flags = r.U8();
                entry.destroyedTimer = r.F32();
                if (entry.flags & FLAG_POSITION)
                {
                    Sample &s = entry.sample;
                    s.x = r.F32();
                    s.y = r.F32();
                    s.aimingAngle = r.F32();
                    s.pause = r.F32();
                    s.ionStun = r.F32();
                    s.weaponCooldown = r.F32();
                    if (entry.flags & FLAG_COMBAT)
                    {
                        s.combat = true;
                        s.heading = r.F32();
                        s.oldHeading = r.F32();
                        s.progress = r.F32();
                    }
                }
            }
            if (!r.Ok()) return false;
            g_drones.pending.swap(entries);
            g_drones.havePending = true;
            return true;
        }

        // Launches or powers a puppet the way the drone button does. The owner already paid the drone part.
        static void PowerPuppet(ShipManager *replica, Drone *drone, Puppet &puppet)
        {
            DroneSystem *system = replica->droneSystem;
            bool launching = !drone->deployed;
            if (launching && system->drone_count <= 0) system->drone_count = 1;
            drone->destroyedTimer = 0.f;
            if (!replica->PowerDrone(drone, 1, false, false) && !puppet.launchFailedLogged)
            {
                puppet.launchFailedLogged = true;
                Log("Drones: the replica's %s could not be %s (drone system power %d/%d, reactor %d/%d)",
                    drone->blueprint ? drone->blueprint->name.c_str() : "drone", launching ? "launched" : "powered",
                    system->powerState.first, system->powerState.second,
                    PowerManager::GetPowerManager(1)->currentPower.first, PowerManager::GetPowerManager(1)->currentPower.second);
            }
        }

        void ApplyState(double localTime)
        {
            if (!g_drones.havePending) return;
            g_drones.havePending = false;
            // Rises at once with a late update, settles slowly: the puppets' delay follows the worst recent latency.
            double age = std::max(0.0, WallMs() - localTime);
            double &smoothed = g_drones.updateAgeMs;
            smoothed = smoothed < 0.0 || age > smoothed ? age : smoothed * 0.98 + age * 0.02;
            ShipManager *replica = G_->GetShipManager(1);
            if (!replica || !replica->droneSystem) return;
            std::vector<Drone*> &drones = replica->droneSystem->drones;
            if (g_drones.puppets.size() != drones.size()) g_drones.puppets.resize(drones.size());

            for (size_t slot = 0; slot < drones.size() && slot < g_drones.pending.size(); ++slot)
            {
                Drone *drone = drones[slot];
                SpaceDrone *space = AsSpaceDrone(drone);
                if (!space) continue;
                const Entry &entry = g_drones.pending[slot];
                Puppet &puppet = g_drones.puppets[slot];
                bool ownerDead = (entry.flags & FLAG_DEAD) != 0;
                bool ownerDeployed = (entry.flags & FLAG_DEPLOYED) != 0 && !ownerDead;
                bool ownerPowered = (entry.flags & FLAG_POWERED) != 0;
                bool wasDeployed = drone->deployed;

                if (entry.flags & FLAG_EXPLODING)
                {
                    // Hit: it explodes as the owner's did; the replica's drone system takes it in when the blast is over.
                    if (drone->deployed && !drone->bDead && !space->explosion.tracker.running) space->BlowUp(false);
                }
                else if (!space->explosion.tracker.running)
                {
                    drone->destroyedTimer = entry.destroyedTimer;   // the rebuild bar
                    if (ownerDeployed && !drone->deployed)
                    {
                        PowerPuppet(replica, drone, puppet);
                        if (!ownerPowered && drone->powered) replica->DePowerDrone(drone, false);
                    }
                    else if (ownerDeployed && ownerPowered && !drone->powered)
                    {
                        PowerPuppet(replica, drone, puppet);
                    }
                    else if (ownerDeployed && !ownerPowered && drone->powered)
                    {
                        replica->DePowerDrone(drone, false);
                    }
                    else if (!ownerDeployed && drone->deployed)
                    {
                        // Gone without a blast (a hull repair drone after its last repair), or taken back: FTL only
                        // recalls the player's drones by itself.
                        if (drone->powered) replica->DePowerDrone(drone, false);
                        drone->SetDeployed(false);
                    }
                    drone->bDead = ownerDead;
                }

                if (drone->deployed && !wasDeployed)
                {
                    puppet.samples.clear();
                    puppet.placed = false;
                    puppet.launchFailedLogged = false;
                }
                if (entry.flags & FLAG_POSITION)
                {
                    Sample sample = entry.sample;
                    sample.t = localTime;
                    if (!puppet.samples.empty() && sample.t <= puppet.samples.back().t) sample.t = puppet.samples.back().t + 1.0;
                    puppet.samples.push_back(sample);
                    while (puppet.samples.size() > MAX_SAMPLES) puppet.samples.pop_front();
                }
                else
                {
                    puppet.samples.clear();
                }
                puppet.flags = entry.flags;
            }
            // The owner's drone parts, after the launches above took theirs.
            replica->droneSystem->drone_count = std::max(0, g_drones.pendingParts);
        }

        // --------------------------------------------------------------------------------------------------------
        // Puppets follow their owners
        // --------------------------------------------------------------------------------------------------------

        static float LerpAngle(float a, float b, float f)
        {
            float d = std::fmod(b - a + 540.f, 360.f) - 180.f;
            float angle = a + d * f;
            if (angle < 0.f) angle += 360.f;
            if (angle >= 360.f) angle -= 360.f;
            return angle;
        }

        // How a puppet's place was found this frame (for the trace).
        enum class Fit { Before, Between, Ahead, Held };

        static Sample Interpolate(const std::deque<Sample> &samples, double t, Fit &fit)
        {
            fit = Fit::Before;
            if (t <= samples.front().t) return samples.front();
            const Sample &last = samples.back();
            fit = Fit::Held;
            if (t > last.t && samples.size() >= 2)
            {
                // No update yet for this moment (a lost or late one): on along the last movement, for a while.
                const Sample &before = samples[samples.size() - 2];
                double span = last.t - before.t;
                if (span <= 1.0) return last;
                fit = t - last.t <= MAX_EXTRAPOLATION_MS ? Fit::Ahead : Fit::Held;
                float f = (float)(std::min(t - last.t, MAX_EXTRAPOLATION_MS) / span);
                Sample s = last;
                s.x = last.x + (last.x - before.x) * f;
                s.y = last.y + (last.y - before.y) * f;
                return s;
            }
            for (size_t i = 1; i < samples.size(); ++i)
            {
                const Sample &b = samples[i];
                if (b.t < t) continue;
                const Sample &a = samples[i - 1];
                float f = (float)((t - a.t) / std::max(1.0, b.t - a.t));
                fit = Fit::Between;
                Sample s = b;
                s.x = a.x + (b.x - a.x) * f;
                s.y = a.y + (b.y - a.y) * f;
                s.aimingAngle = LerpAngle(a.aimingAngle, b.aimingAngle, f);
                s.pause = a.pause + (b.pause - a.pause) * f;
                s.ionStun = a.ionStun + (b.ionStun - a.ionStun) * f;
                s.weaponCooldown = a.weaponCooldown + (b.weaponCooldown - a.weaponCooldown) * f;
                if (a.combat && a.heading == b.heading && a.oldHeading == b.oldHeading) s.progress = a.progress + (b.progress - a.progress) * f;
                return s;
            }
            return samples.back();
        }

        static void PruneTracked(std::vector<TrackedShot> &shots, const std::set<Projectile*> &live)
        {
            shots.erase(std::remove_if(shots.begin(), shots.end(), [&](const TrackedShot &shot) {
                return !live.count(shot.projectile) || shot.projectile->selfId != shot.selfId;
            }), shots.end());
        }

        void AfterSpaceLoop()
        {
            WorldManager *world = G_->GetWorld();
            if (!world) return;
            if (!g_drones.visual.empty() || !g_drones.own.empty())
            {
                std::set<Projectile*> live(world->space.projectiles.begin(), world->space.projectiles.end());
                PruneTracked(g_drones.visual, live);
                PruneTracked(g_drones.own, live);
            }

            ShipManager *replica = G_->GetShipManager(1);
            if (!replica || !replica->droneSystem || !Replaced()) return;
            std::vector<Drone*> &drones = replica->droneSystem->drones;
            if (g_drones.puppets.size() != drones.size()) g_drones.puppets.resize(drones.size());
            double delay = std::max(0.0, g_drones.updateAgeMs) + UPDATE_INTERVAL_MS + JITTER_ROOM_MS;
            double renderTime = WallMs() - delay;

            for (size_t slot = 0; slot < drones.size(); ++slot)
            {
                SpaceDrone *space = AsSpaceDrone(drones[slot]);
                if (!space || !space->deployed || space->bDead) continue;
                // A puppet never fires, repairs or runs out on its own; its owner's game does all that.
                space->bFire = false;
                space->lifespan = INT_MAX;
                Puppet &puppet = g_drones.puppets[slot];
                if (puppet.samples.empty() || space->explosion.tracker.running) continue;

                Fit fit;
                Sample s = Interpolate(puppet.samples, renderTime, fit);
                if (GetState().trace)
                {
                    static const char *const FITS[] = {"before", "between", "ahead", "held"};
                    if (!g_drones.trace.IsOpen()) g_drones.trace.Open("duels_drones.csv", "wall_ms,slot,x,y,fit,delay_ms,updates");
                    Row row;
                    row << WallMs() << slot << s.x << s.y << FITS[(int)fit] << delay << puppet.samples.size();
                    g_drones.trace.WriteRow(row.str());
                }
                // Combat drones fly around the opponent's target (our ship, space 0), the others around their own ship.
                int spaceId = (puppet.flags & FLAG_TARGET_SPACE) ? 0 : 1;
                space->currentSpace = spaceId;
                space->destinationSpace = spaceId;
                Pointf location(s.x, s.y);
                space->lastLocation = puppet.placed ? space->currentLocation : location;
                space->currentLocation = location;
                space->speedVector = Pointf(location.x - space->lastLocation.x, location.y - space->lastLocation.y);
                space->aimingAngle = s.aimingAngle;
                space->pause = s.pause;
                space->ionStun = s.ionStun;
                space->weaponCooldown = s.weaponCooldown;
                if (s.combat && CombatLike(space))
                {
                    CombatDrone *combat = static_cast<CombatDrone*>(space);
                    combat->heading = s.heading;
                    combat->oldHeading = s.oldHeading;
                    combat->progressToDestination = s.progress;
                }
                puppet.placed = true;
            }
        }

        // --------------------------------------------------------------------------------------------------------
        // Shots and hits
        // --------------------------------------------------------------------------------------------------------

        bool MayFire(SpaceDrone *drone)
        {
            return !IsPuppet(drone);
        }

        static bool Tracked(const std::vector<TrackedShot> &shots, const Projectile *projectile)
        {
            for (const TrackedShot &shot : shots)
            {
                if (shot.projectile == projectile && shot.selfId == projectile->selfId) return true;
            }
            return false;
        }

        bool IsVisualShot(const Projectile *projectile)
        {
            return projectile && Tracked(g_drones.visual, projectile);
        }

        bool IsOwnDroneShot(const Projectile *projectile)
        {
            return projectile && Tracked(g_drones.own, projectile);
        }

        void OnOwnDroneShotInOwnSpace(SpaceDrone *drone, Projectile *projectile)
        {
            TrackedShot shot;
            shot.projectile = projectile;
            shot.selfId = projectile->selfId;
            shot.target = projectile->target;
            g_drones.own.push_back(shot);
            if (!Net::IsConnected() || !drone->weaponBlueprint) return;

            int slot = SlotOf(G_->GetShipManager(0), drone);
            if (slot < 0) return;
            Writer w;
            w.U8((uint8_t)slot);
            w.Str(drone->weaponBlueprint->name);
            w.F32(projectile->position.x);
            w.F32(projectile->position.y);
            w.F32(projectile->target.x);
            w.F32(projectile->target.y);
            Net::Send(MSG_DRONE_SHOT, w, true);
        }

        // A harmless copy of the opponent's drone shot, from where our screen shows their drone.
        static void CreateShotCopy(Reader &r)
        {
            int slot = r.U8();
            std::string weaponName = r.Str();
            Pointf origin, target;
            origin.x = r.F32();
            origin.y = r.F32();
            target.x = r.F32();
            target.y = r.F32();
            if (!r.Ok()) return;
            ShipManager *replica = G_->GetShipManager(1);
            WeaponBlueprint *blueprint = G_->GetBlueprints()->GetWeaponBlueprint(weaponName);
            if (!replica || !blueprint || blueprint->name != weaponName || G_->GetWorld() == nullptr) return;
            SpaceDrone *puppet = DroneInSlot(replica, slot);
            if (puppet && puppet->deployed && !puppet->bDead && puppet->currentSpace == 1) origin = puppet->currentLocation;

            LaserBlast *laser = new LaserBlast(origin, 1, 1, target);
            laser->heading = -1.f;
            laser->OnInit();
            laser->Initialize(*blueprint);
            laser->ownerId = 1;
            if (puppet) laser->flight_animation = puppet->weapon_animation;
            G_->GetWorld()->space.AddProjectile(laser);
            if (!blueprint->effects.launchSounds.empty())
            {
                G_->GetSoundControl()->PlaySoundMix(blueprint->effects.launchSounds[random32() % blueprint->effects.launchSounds.size()], -1.f, false);
            }
            TrackedShot shot;
            shot.projectile = laser;
            shot.selfId = laser->selfId;
            shot.target = target;
            g_drones.visual.push_back(shot);
            ++g_drones.shotCopies;
        }

        static void EndShot(Projectile *projectile)
        {
            if (projectile->startedDeath) return;
            projectile->death_animation.Start(true);
            projectile->startedDeath = true;
            projectile->missed = true;
        }

        void EndVisualShotsNear(float x, float y, int space)
        {
            for (const TrackedShot &shot : g_drones.visual)
            {
                Projectile *projectile = shot.projectile;
                if (projectile->currentSpace != space) continue;
                float dx = projectile->position.x - x, dy = projectile->position.y - y;
                float tx = shot.target.x - x, ty = shot.target.y - y;
                if (dx * dx + dy * dy < 60.f * 60.f || tx * tx + ty * ty < 30.f * 30.f) EndShot(projectile);
            }
        }

        void ObserveDroneCollision(SpaceDrone *drone, bool explodingBefore, float ionStunBefore)
        {
            // Only the opponent's drones in our space are ours to judge (theirs in their space never collide here).
            if (!IsPuppet(drone) || drone->currentSpace != 0 || !Net::IsConnected()) return;
            int slot = SlotOf(G_->GetShipManager(1), drone);
            if (slot < 0) return;
            Writer w;
            w.U8((uint8_t)slot);
            if (!explodingBefore && drone->explosion.tracker.running)
            {
                w.U8(HIT_DESTROYED);
                w.F32(0.f);
            }
            else if (drone->ionStun > ionStunBefore)
            {
                w.U8(HIT_ION);
                w.F32(drone->ionStun);
            }
            else
            {
                return;
            }
            Net::Send(MSG_DRONE_HIT, w, true);
            ++g_drones.hitsSent;
            Log("Drones: the opponent's drone in slot %d was %s in our space", slot,
                drone->explosion.tracker.running ? "destroyed" : "ionized");
        }

        // The opponent hit one of our drones in their space: our drone takes it.
        static void ApplyHit(Reader &r)
        {
            int slot = r.U8();
            int kind = r.U8();
            float ionStun = r.F32();
            if (!r.Ok()) return;
            ++g_drones.hitsReceived;
            SpaceDrone *drone = DroneInSlot(G_->GetShipManager(0), slot);
            if (!drone || !drone->deployed || drone->bDead || drone->explosion.tracker.running) return;
            if (kind == HIT_DESTROYED)
            {
                drone->BlowUp(false);
                EndVisualShotsNear(drone->currentLocation.x, drone->currentLocation.y, drone->currentSpace);
            }
            else if (kind == HIT_ION && drone->powered)
            {
                drone->ionStun = std::max(drone->ionStun, ionStun);
                EndVisualShotsNear(drone->currentLocation.x, drone->currentLocation.y, drone->currentSpace);
            }
            Log("Drones: our drone in slot %d was %s by the opponent", slot, kind == HIT_DESTROYED ? "destroyed" : "ionized");
        }

        void OnMessage(uint8_t type, Reader &r)
        {
            if (type == MSG_DRONE_HIT) ApplyHit(r);
            else if (type == MSG_DRONE_SHOT) CreateShotCopy(r);
        }

        bool RunsOwnLoop(Drone *drone)
        {
            return !IsPuppet(drone);
        }

        bool BlocksReplicaRepair(ShipManager *ship, int damage)
        {
            return ship && ship->iShipId == 1 && Replaced() && damage < 0;
        }

        std::string Status()
        {
            std::ostringstream out;
            out << "drone hits reported " << g_drones.hitsSent << " received " << g_drones.hitsReceived
                << ", drone shot copies " << g_drones.shotCopies;
            return out.str();
        }

        std::string Signature(ShipManager *ship)
        {
            std::ostringstream out;
            if (!ship || !ship->droneSystem) return out.str();
            out << ship->GetDroneCount() << ':';
            for (Drone *drone : ship->droneSystem->drones)
            {
                SpaceDrone *space = AsSpaceDrone(drone);
                bool wrecked = space && (drone->bDead || space->explosion.tracker.running);
                out << (drone->powered ? 'P' : 'p') << (drone->deployed ? 'D' : 'd');
                if (wrecked) out << 'X';
                else if (drone->destroyedTimer > 0.f) out << 'R';
                out << ' ';
            }
            return out.str();
        }
    }
}
