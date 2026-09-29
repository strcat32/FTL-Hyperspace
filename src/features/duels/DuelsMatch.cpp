#include "Global.h"
#include "CommandConsole.h"
#include "CustomDamage.h"
#include "HSVersion.h"
#include "Systems.h"
#include "Duels.h"
#include "DuelsConsole.h"
#include "DuelsBays.h"
#include "DuelsCrew.h"
#include "DuelsDrones.h"
#include "DuelsMatch.h"
#include "DuelsRooms.h"
#include "DuelsNet.h"
#include "DuelsShipControl.h"
#include "DuelsTrace.h"
#include "DuelsView.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace Duels
{
    namespace Match
    {
        // Game messages (Net reserves types below 16 for the session).
        enum GameMessage : uint8_t
        {
            MSG_CHAT = 16,
            MSG_LOADOUT = 17,    // reliable: the ship to build as the replica
            MSG_READY = 18,      // reliable: the replica is built
            MSG_STATE = 19,      // unreliable, 10 Hz: hull, shields, systems, weapons
            MSG_SHOT = 20,       // reliable: a projectile left one of our weapons
            MSG_RESULT = 21,     // reliable: the defender's verdict on a shot
            MSG_DEFEAT = 22,     // reliable: our hull reached 0
            MSG_SHOT_DOWNED = 23, // reliable: our shot ran into something in our own space before it left
            MSG_SETTINGS = 27    // reliable, host to guest: the duel's settings (crew experience)
            // 24, 25: DuelsDrones.h
        };

        enum Outcome : uint8_t
        {
            PENDING = 0,
            OUTCOME_MISS = 1,     // dodged
            OUTCOME_SHIELD = 2,   // absorbed by the shields
            OUTCOME_HIT = 3,      // reached the hull
            OUTCOME_GONE = 4,     // the defender never saw it hit anything
            OUTCOME_DOWNED = 5    // exploded before the shields: shot down by a defense drone, or ran into a drone
        };

        static const char *OutcomeName(uint8_t outcome)
        {
            switch (outcome)
            {
            case OUTCOME_MISS: return "miss";
            case OUTCOME_SHIELD: return "shield";
            case OUTCOME_HIT: return "hit";
            case OUTCOME_GONE: return "gone";
            case OUTCOME_DOWNED: return "downed";
            default: return "pending";
            }
        }

        // Weapon blueprint types (CustomWeapons.cpp). Flak ("burst") fires laser blasts at scattered points.
        static const int WEAPON_LASER = 0;
        static const int WEAPON_MISSILES = 1;
        static const int WEAPON_BEAM = 2;
        static const int WEAPON_BOMB = 3;
        static const int WEAPON_BURST = 4;

        static bool Networked(int type)
        {
            return type >= WEAPON_LASER && type <= WEAPON_BURST;
        }

        static const double STATE_INTERVAL_MS = 100.0;
        static const double HOLD_TIMEOUT_MS = 3000.0;     // no verdict by then: show a miss, the state has the truth
        static const double BEAM_VERDICT_WAIT_MS = 3000.0;
        static const double NATURAL_OPPONENT_LEG_MS = 1100.0;   // step 1: a shot's flight out of the enemy window
        static const int MAX_SLOTS = 8;

        // Our projectile flying at the opponent's replica.
        struct OutShot
        {
            Projectile *projectile = nullptr;
            unsigned int selfId = 0;
            uint32_t netId = 0;
            int slot = 0;
            int type = WEAPON_LASER;
            std::string weapon;
            double spawnMs = 0.0;
            double transferMs = -1.0;
            double holdStartMs = -1.0;
            double verdictMs = -1.0;
            uint8_t verdict = PENDING;
            int damage = 0;
            bool timedOut = false;
            double goneMs = -1.0;       // a beam whose sweep is over, still waiting for its verdict
            bool drone = false;         // fired by one of our drones (slot = the drone's slot)
            Pointf downPoint;           // "downed": where it exploded on the defender's screen
            float downDistance = 1.0e9f;
            bool exploded = false;
        };

        // The opponent's projectile flying at our ship, created from their shot message.
        struct InShot
        {
            Projectile *projectile = nullptr;
            unsigned int selfId = 0;
            uint32_t netId = 0;
            int type = WEAPON_LASER;
            bool beamTouched = false;      // a beam reached our shields
            bool beamHit = false;          // a beam reached a room
            bool drone = false;            // fired by one of the opponent's drones, in our space
            double catchUpMs = 0.0;        // a drone's shot makes up the message's delay this way, flying faster
            std::string weapon;
            double spawnMs = 0.0;          // the attacker's spawn time, in our clock
            double receivedMs = 0.0;
            double transferMs = -1.0;
            double releaseAt = 0.0;
            double releasedMs = -1.0;
            double decisionMs = -1.0;
            int speedUp = 1;
            bool released = false;
            uint8_t outcome = PENDING;
            int damage = 0;
        };

        struct MatchState
        {
            bool initialised = false;
            std::string playerName = "Captain";

            bool loadoutSent = false;
            std::string sentArmament;      // our weapons and drones, in slot order, as the last loadout had them
            bool replicaReady = false;     // we built the opponent's ship
            bool peerReady = false;        // they built ours
            bool defeatSent = false;
            std::string opponentShip;

            uint16_t stateSeq = 0;
            bool havePeerState = false;
            uint16_t peerStateSeq = 0;
            double lastStateSent = -1.0e9;
            bool stateDirty = false;
            uint32_t statesApplied = 0;
            std::map<int, int> subsystemPower;   // the owner's, by system id (piloting, sensors, doors, battery)

            uint32_t nextNetId = 1;
            std::vector<OutShot> out;
            std::vector<InShot> in;
            double legEstimate[MAX_SLOTS];
            double lastFireMs[MAX_SLOTS];   // when a replica weapon last fired a received shot

            uint32_t shotsSent = 0, shotsReceived = 0, verdictsSent = 0, verdictsReceived = 0, holdTimeouts = 0;
            uint32_t hullKept = 0;         // hits whose copy would have taken the replica's last hull point
            std::set<std::string> unsupportedLogged;

            CsvFile shotsCsv;
            CsvFile syncCsv;
            std::string lastSignature[2];
        };

        static MatchState g_match;
        static OutShot *g_forced = nullptr;   // our shot whose collision check is running with a verdict

        static void ResetMatch()
        {
            MatchState &m = g_match;
            m.loadoutSent = false;
            m.replicaReady = false;
            m.peerReady = false;
            m.defeatSent = false;
            m.opponentShip.clear();
            m.havePeerState = false;
            m.stateDirty = false;
            m.statesApplied = 0;
            m.subsystemPower.clear();
            m.out.clear();
            m.in.clear();
            for (int i = 0; i < MAX_SLOTS; ++i)
            {
                m.legEstimate[i] = -1.0;
                m.lastFireMs[i] = -1.0e9;
            }
            m.lastSignature[0].clear();
            m.lastSignature[1].clear();
            g_forced = nullptr;
            Drones::Reset();
            Crew::Reset();
            Rooms::Reset();
        }

        static void Announce(const std::string &text)
        {
            Log("Match: %s", text.c_str());
            Console::Print("DUEL: " + text);
        }

        static bool InGame()
        {
            WorldManager *world = G_->GetWorld();
            CApp *app = G_->GetCApp();
            return world && world->playerShip && world->commandGui && app && !app->menu.bOpen;
        }

        static int WeaponSlot(ShipManager *ship, ProjectileFactory *weapon)
        {
            if (!ship || !ship->weaponSystem) return -1;
            std::vector<ProjectileFactory*> weapons = ship->GetWeaponList();
            for (size_t slot = 0; slot < weapons.size(); ++slot)
            {
                if (weapons[slot] == weapon) return (int)slot;
            }
            return -1;
        }

        static OutShot *FindOut(Projectile *projectile)
        {
            for (OutShot &shot : g_match.out)
            {
                if (shot.projectile == projectile && shot.selfId == projectile->selfId) return &shot;
            }
            return nullptr;
        }

        static InShot *FindIn(Projectile *projectile)
        {
            if (!projectile) return nullptr;
            for (InShot &shot : g_match.in)
            {
                if (shot.projectile == projectile && shot.selfId == projectile->selfId) return &shot;
            }
            return nullptr;
        }

        static void LogShot(const char *side, uint32_t netId, const std::string &weapon, double spawn, double received,
                            double transfer, double release, double holdStart, double decision, double verdict,
                            uint8_t outcome, int damage, int speedUp)
        {
            if (!g_match.shotsCsv.IsOpen())
            {
                g_match.shotsCsv.Open("duels_shots.csv",
                                      "side,net_id,weapon,spawn_ms,received_ms,transfer_ms,release_ms,hold_start_ms,"
                                      "decision_ms,verdict_ms,held_ms,outcome,damage,speed_up,clock_offset_ms,rtt_ms");
            }
            double held = holdStart >= 0.0 && verdict >= 0.0 ? verdict - holdStart : 0.0;
            Row row;
            row << side << netId << weapon << spawn << received << transfer << release << holdStart << decision << verdict
                << held << OutcomeName(outcome) << damage << speedUp << (Net::HasClock() ? Net::LocalToPeerTime(0.0) : 0.0)
                << Net::RttMs();
            g_match.shotsCsv.WriteRow(row.str());
        }

        // --------------------------------------------------------------------------------------------------------
        // Loadout: what the opponent needs to build our ship
        // --------------------------------------------------------------------------------------------------------

        // Our weapons and drones in slot order. When it changes (weapons dragged to other slots, a refit), the
        // opponent gets the loadout again: slots decide power, charge and the weapon bays.
        static std::string Armament(ShipManager *ship)
        {
            std::string text;
            if (!ship) return text;
            if (ship->weaponSystem)
            {
                for (ProjectileFactory *weapon : ship->GetWeaponList()) text += (weapon->blueprint ? weapon->blueprint->name : "?") + ",";
            }
            text += "|";
            if (ship->droneSystem)
            {
                for (Drone *drone : ship->GetDroneList()) text += (drone->blueprint ? drone->blueprint->name : "?") + ",";
            }
            return text;
        }

        static void SendLoadout()
        {
            ShipManager *ship = G_->GetShipManager(0);
            if (!ship) return;
            Writer w;
            w.Str(ship->myBlueprint.blueprintName);
            w.I16((int16_t)ship->ship.hullIntegrity.second);
            w.I16((int16_t)ship->ship.hullIntegrity.first);
            w.U8((uint8_t)PowerManager::GetPowerManager(0)->currentPower.second);

            w.U8((uint8_t)ship->vSystemList.size());
            for (ShipSystem *system : ship->vSystemList)
            {
                w.U8((uint8_t)system->iSystemType);
                w.U8((uint8_t)system->powerState.second);
            }
            std::vector<ProjectileFactory*> weapons = ship->weaponSystem ? ship->GetWeaponList() : std::vector<ProjectileFactory*>();
            w.U8((uint8_t)weapons.size());
            for (ProjectileFactory *weapon : weapons) w.Str(weapon->blueprint ? weapon->blueprint->name : "");
            w.U8((uint8_t)ship->vCrewList.size());
            for (CrewMember *crew : ship->vCrewList) w.Str(crew->species);
            // Drones in slot order (drone messages refer to slots), and the drone parts.
            std::vector<Drone*> drones = ship->droneSystem ? ship->GetDroneList() : std::vector<Drone*>();
            w.U8((uint8_t)drones.size());
            for (Drone *drone : drones) w.Str(drone->blueprint ? drone->blueprint->name : "");
            w.I16((int16_t)ship->GetDroneCount());

            Net::Send(MSG_LOADOUT, w, true);
            g_match.loadoutSent = true;
            g_match.sentArmament = Armament(ship);
            Log("Match: loadout sent (%s, hull %d/%d, %u systems, %u weapons, %u drones)", ship->myBlueprint.blueprintName.c_str(),
                ship->ship.hullIntegrity.first, ship->ship.hullIntegrity.second, (unsigned)ship->vSystemList.size(),
                (unsigned)weapons.size(), (unsigned)drones.size());
        }

        static void ApplyLoadout(Reader &r)
        {
            std::string blueprint = r.Str();
            int hullMax = r.I16();
            int hull = r.I16();
            int reactor = r.U8();
            struct SystemLevel { int id; int level; };
            std::vector<SystemLevel> systems(r.U8());
            for (SystemLevel &system : systems)
            {
                system.id = r.U8();
                system.level = r.U8();
            }
            std::vector<std::string> weapons(r.U8());
            for (std::string &weapon : weapons) weapon = r.Str();
            std::vector<std::string> crew(r.U8());
            for (std::string &species : crew) species = r.Str();
            std::vector<std::string> drones(r.U8());
            for (std::string &drone : drones) drone = r.Str();
            int droneParts = r.I16();
            if (!r.Ok())
            {
                Log("Match: malformed loadout");
                return;
            }

            if (!InGame())
            {
                Log("Match: loadout received outside a game; ignored");
                return;
            }
            ShipManager *replica = G_->GetShipManager(1);
            if (replica && replica->myBlueprint.blueprintName != blueprint)
            {
                Log("Match: an enemy (%s) is already present; it stays", replica->myBlueprint.blueprintName.c_str());
            }
            // The replica's shields go where the player's own ship has them (DuelsView) from the moment it exists:
            // our combat drones pick their orbit from its shield ellipse as soon as it is our target.
            View::UsePlayerShieldPosition(nullptr);
            if (!replica)
            {
                std::string message;
                if (!SpawnEnemy(blueprint, message))
                {
                    Log("Match: cannot build the opponent's ship: %s", message.c_str());
                    return;
                }
                replica = G_->GetShipManager(1);
                if (!replica) return;
            }

            // The replica belongs to the network from now on: no AI, no pause, no local repower.
            State &state = GetState();
            state.aiOff[1] = true;
            state.noPause = true;

            replica->ship.hullIntegrity.second = hullMax;
            replica->ship.hullIntegrity.first = hull;
            PowerManager::GetPowerManager(1)->currentPower.second = reactor;

            for (const SystemLevel &level : systems)
            {
                if (!replica->HasSystem(level.id))
                {
                    replica->AddSystem(level.id);
                    if (!replica->HasSystem(level.id))
                    {
                        Log("Match: the replica cannot get %s", SystemName(level.id));
                        continue;
                    }
                }
                ShipSystem *system = replica->GetSystem(level.id);
                int difference = level.level - system->powerState.second;
                if (difference > 0) replica->UpgradeSystem(level.id, difference);
                if (system->powerState.second != level.level)
                {
                    Log("Match: replica %s is level %d, the owner's is %d", SystemName(level.id), system->powerState.second,
                        level.level);
                }
            }

            // Weapons in the owner's slot order; shots refer to slots.
            std::vector<ProjectileFactory*> current = replica->weaponSystem ? replica->GetWeaponList() : std::vector<ProjectileFactory*>();
            bool same = current.size() == weapons.size();
            for (size_t slot = 0; same && slot < weapons.size(); ++slot)
            {
                same = current[slot]->blueprint && current[slot]->blueprint->name == weapons[slot];
            }
            if (!same && replica->weaponSystem)
            {
                for (int slot = (int)current.size() - 1; slot >= 0; --slot) replica->RemoveWeapon(slot);
                for (size_t slot = 0; slot < weapons.size(); ++slot)
                {
                    WeaponBlueprint *weapon = G_->GetBlueprints()->GetWeaponBlueprint(weapons[slot]);
                    if (weapon) replica->AddWeapon(weapon, (int)slot);
                }
                Log("Match: replica weapons replaced (%u)", (unsigned)weapons.size());
            }

            // Drones likewise.
            std::vector<Drone*> currentDrones = replica->droneSystem ? replica->GetDroneList() : std::vector<Drone*>();
            bool sameDrones = currentDrones.size() == drones.size();
            for (size_t slot = 0; sameDrones && slot < drones.size(); ++slot)
            {
                sameDrones = currentDrones[slot]->blueprint && currentDrones[slot]->blueprint->name == drones[slot];
            }
            if (!sameDrones && replica->droneSystem)
            {
                for (int slot = (int)currentDrones.size() - 1; slot >= 0; --slot) replica->RemoveDrone(slot);
                for (size_t slot = 0; slot < drones.size(); ++slot)
                {
                    DroneBlueprint *drone = G_->GetBlueprints()->GetDroneBlueprint(drones[slot]);
                    if (drone) replica->AddDrone(drone, (int)slot);
                }
                Log("Match: replica drones replaced (%u)", (unsigned)drones.size());
            }
            if (replica->droneSystem) replica->ModifyDroneCount(droneParts - replica->GetDroneCount());

            View::UsePlayerShieldPosition(replica);
            // Combat drones already bound to it took their waypoint from its shields before they moved; they take a
            // new one around the right ellipse (else the first shot can start inside the shields).
            if (ShipManager *own = G_->GetShipManager(0))
            {
                for (SpaceDrone *drone : own->spaceDrones)
                {
                    if (drone->type == 1 && drone->movementTarget == &replica->_targetable) drone->SetMovementTarget(&replica->_targetable);
                }
            }
            g_match.opponentShip = blueprint;
            g_match.replicaReady = true;
            Net::Send(MSG_READY, Writer(), true);
            Announce("opponent's ship " + blueprint + " is here");
        }

        // --------------------------------------------------------------------------------------------------------
        // State: our ship ten times a second; the replica follows
        // --------------------------------------------------------------------------------------------------------

        // A system's power bars as its owner set them: from the reactor and from a running battery. (Bonus power,
        // from Zoltan crew for example, comes with the crew on each side.)
        static int PowerBars(const ShipSystem *system)
        {
            return system->powerState.first + system->iBatteryPower;
        }

        // Sets a replica system's power bars to the owner's, with the owner's lock already on it. Power goes down the
        // way ion and damage take it (which passes locks; shields keep a lone bar only while locked, as the owner's
        // did), and up the way the power bars add it.
        static void SetReplicaPower(ShipManager *replica, ShipSystem *system, int level)
        {
            if (!system->bNeedsPower)
            {
                // Subsystems (piloting, sensors, doors, battery) need no reactor power, and their power can't be
                // changed by hand; only the environment changes it (a nebula switches the sensors off). This game's
                // loop sets it again every frame from this game's environment, so HoldReplicaSubsystems holds it.
                system->powerState.first = std::max(0, std::min(level, system->powerState.second));
                return;
            }
            for (int guard = 0; guard < 32 && PowerBars(system) != level; ++guard)
            {
                bool changed;
                if (PowerBars(system) > level)
                {
                    changed = system->ForceDecreasePower(1);
                }
                else
                {
                    // The owner's power only rises under a lock as it ends; lift the replica's for the step.
                    int lock = system->iLockCount;
                    system->iLockCount = 0;
                    changed = replica->IncreaseSystemPower(system->iSystemType);
                    system->iLockCount = lock;
                }
                if (!changed) break;
            }
        }

        void HoldReplicaSubsystems(ShipManager *ship)
        {
            if (!ship || ship->iShipId != 1 || !g_match.replicaReady || ship != G_->GetShipManager(1)) return;
            Crew::AfterReplicaLoop(ship);
            Rooms::AfterReplicaLoop(ship);
            for (const std::pair<const int, int> &entry : g_match.subsystemPower)
            {
                ShipSystem *system = ship->GetSystem(entry.first);
                if (system && !system->bNeedsPower)
                {
                    system->powerState.first = std::max(0, std::min(entry.second, system->powerState.second));
                }
            }
        }

        // The replica's lock timers run this far behind the owner's, so a lock ends when the owner's update says so,
        // not by the replica's own count (that would repower the system on its own).
        static const float LOCK_TIMER_LAG_S = 0.25f;

        struct SystemState { int id; int power; int health; int lock; float lockTime; float lockGoal; };

        static void ApplyLocks(ShipManager *replica, const std::vector<SystemState> &systems)
        {
            for (const SystemState &state : systems)
            {
                ShipSystem *system = replica->GetSystem(state.id);
                if (!system) continue;
                system->iLockCount = state.lock;
                if (state.lock > 0)
                {
                    system->lockTimer.running = true;
                    system->lockTimer.currTime = std::max(0.f, state.lockTime - LOCK_TIMER_LAG_S);
                    system->lockTimer.currGoal = state.lockGoal;
                }
            }
        }

        // Cloaking (roadmap 2.4). FTL's cloak: on for 5 s per power bar, each shot fired while cloaked takes a fifth
        // off (not with the Stealth Weapons augment), then locked for 4 lock periods; while it runs the ship has 60%
        // more evasion and the enemy's weapons don't charge. Each game's own ship decides all of that; the replica's
        // cloak only follows the owner's: the opponent's weapons stop charging while it is on (Hyperspace's
        // ProjectileFactory::Update asks the target's IsCloaked), and it shows cloaked.
        static bool g_cloakFromOwner = false;   // the state sync is switching the replica's cloak

        bool MaySwitchCloak(const CloakingSystem *cloak)
        {
            if (g_cloakFromOwner || !g_match.replicaReady || !cloak) return true;
            ShipManager *replica = G_->GetShipManager(1);
            return !replica || replica->cloakSystem != cloak;
        }

        int HullDamage(const Ship *ship, int amount)
        {
            ShipManager *replica = g_match.replicaReady ? G_->GetShipManager(1) : nullptr;
            if (!replica || ship != &replica->ship || amount <= 0) return amount;
            int keep = std::max(0, ship->hullIntegrity.first - 1);
            if (amount <= keep) return amount;
            ++g_match.hullKept;
            return keep;
        }

        static void SetReplicaCloak(CloakingSystem *cloak, bool on, float time, float goal)
        {
            if (cloak->bTurnedOn != on)
            {
                // FTL's own switch: its sound, and the locks it sets (the owner's replace them, ApplyLocks). It
                // refuses a locked cloak: the owner's lock while it runs is already on the replica.
                int lock = cloak->iLockCount;
                cloak->iLockCount = 0;
                g_cloakFromOwner = true;
                cloak->SetTurnedOn(on);
                g_cloakFromOwner = false;
                if (cloak->iLockCount == 0) cloak->iLockCount = lock;
                if (cloak->bTurnedOn != on)
                {
                    // Not working here (no power yet): switched without FTL's function.
                    Log("Match: the replica's cloak did not switch %s by itself (power %d, health %d)", on ? "on" : "off",
                        cloak->GetEffectivePower(), cloak->healthState.first);
                    cloak->bTurnedOn = on;
                }
            }
            if (on)
            {
                // Behind the owner's, like the lock timers.
                cloak->timer.currGoal = goal;
                cloak->timer.currTime = std::max(0.f, time - LOCK_TIMER_LAG_S);
                cloak->timer.running = true;
            }
        }

        // A replica system that could not take its owner's power (logged once per new combination).
        static void LogPowerMiss(const ShipSystem *system, int wanted)
        {
            static std::map<int, std::pair<int, int>> logged;
            std::pair<int, int> now(wanted, PowerBars(system));
            auto found = logged.find(system->iSystemType);
            if (found != logged.end() && found->second == now) return;
            logged[system->iSystemType] = now;
            Log("Match: replica %s has %d power bars (reactor %d, battery %d, bonus %d, effective %d), the owner %d; "
                "reactor %d/%d, battery power %d/%d", SystemName(system->iSystemType), now.second,
                system->powerState.first, system->iBatteryPower, system->iBonusPower,
                const_cast<ShipSystem*>(system)->GetEffectivePower(), wanted, PowerManager::GetPowerManager(1)->currentPower.first,
                PowerManager::GetPowerManager(1)->currentPower.second, PowerManager::GetPowerManager(1)->batteryPower.first,
                PowerManager::GetPowerManager(1)->batteryPower.second);
        }

        static void SendState(double now)
        {
            ShipManager *ship = G_->GetShipManager(0);
            if (!ship) return;
            Writer w;
            w.F64(now);
            w.U16(++g_match.stateSeq);
            w.I16((int16_t)ship->ship.hullIntegrity.first);

            Shields *shields = ship->shieldSystem;
            w.Bool(shields != nullptr);
            if (shields)
            {
                w.U8((uint8_t)std::max(0, shields->shields.power.first));
                w.F32(shields->shields.charger);
                // The Zoltan super shield (Zoltan ships, the shield overcharger drone): its layers and their cap.
                w.U8((uint8_t)std::max(0, std::min(shields->shields.power.super.first, 255)));
                w.U8((uint8_t)std::max(0, std::min(shields->shields.power.super.second, 255)));
            }

            // Per system: power, health and the lock. Ion damage locks a system for one timer period per ion charge
            // (lock count 1-5, its timer counts each period); a running battery holds its own lock at -1, and its
            // cooldown afterwards is a lock like the ion one.
            w.U8((uint8_t)ship->vSystemList.size());
            for (ShipSystem *system : ship->vSystemList)
            {
                w.U8((uint8_t)system->iSystemType);
                w.U8((uint8_t)std::max(0, PowerBars(system)));
                w.U8((uint8_t)std::max(0, system->healthState.first));
                w.I8((int8_t)std::max(-1, std::min(system->iLockCount, 127)));
                if (system->iLockCount > 0)
                {
                    w.F32(system->lockTimer.currTime);
                    w.F32(system->lockTimer.currGoal);
                }
            }

            // The backup battery: on, and how far its 30 seconds have run.
            BatterySystem *battery = ship->batterySystem;
            w.Bool(battery != nullptr);
            if (battery)
            {
                w.Bool(battery->bTurnedOn);
                w.F32(battery->timer.currTime);
            }

            // Cloaking: on, and how far its time has run (its length is the power; each shot fired shortens it).
            CloakingSystem *cloak = ship->cloakSystem;
            w.Bool(cloak != nullptr);
            if (cloak)
            {
                w.Bool(cloak->bTurnedOn);
                w.F32(cloak->timer.currTime);
                w.F32(cloak->timer.currGoal);
            }

            std::vector<ProjectileFactory*> weapons = ship->weaponSystem ? ship->GetWeaponList() : std::vector<ProjectileFactory*>();
            w.U8((uint8_t)weapons.size());
            for (ProjectileFactory *weapon : weapons)
            {
                w.Bool(weapon->powered);
                w.F32(weapon->cooldown.first);
            }

            // Drones: power, launch, wreck and where they are (DuelsDrones.cpp).
            Drones::WriteState(w);
            // Crew: where each one is and how they are (DuelsCrew.cpp); who is on board goes first, when it changed.
            if (Crew::RosterChanged())
            {
                Writer roster;
                Crew::WriteRoster(roster);
                Net::Send(Crew::MSG_CREW_ROSTER, roster, true);
            }
            Crew::WriteState(w);
            // Rooms: oxygen, fires, breaches, doors, lockdowns (DuelsRooms.cpp).
            Rooms::WriteState(w);

            Net::Send(MSG_STATE, w, false);
            g_match.lastStateSent = now;
            g_match.stateDirty = false;
        }

        static void ApplyState(Reader &r)
        {
            double sentAt = r.F64();
            uint16_t seq = r.U16();
            int hull = r.I16();
            bool hasShields = r.Bool();
            int shieldLayers = 0;
            float shieldCharge = 0.f;
            int superShield = 0, superShieldMax = 0;
            if (hasShields)
            {
                shieldLayers = r.U8();
                shieldCharge = r.F32();
                superShield = r.U8();
                superShieldMax = r.U8();
            }
            std::vector<SystemState> systems(r.U8());
            for (SystemState &system : systems)
            {
                system.id = r.U8();
                system.power = r.U8();
                system.health = r.U8();
                system.lock = r.I8();
                system.lockTime = system.lock > 0 ? r.F32() : 0.f;
                system.lockGoal = system.lock > 0 ? r.F32() : 0.f;
            }
            bool hasBattery = r.Bool();
            bool batteryOn = hasBattery && r.Bool();
            float batteryTime = hasBattery ? r.F32() : 0.f;
            bool hasCloak = r.Bool();
            bool cloakOn = hasCloak && r.Bool();
            float cloakTime = hasCloak ? r.F32() : 0.f;
            float cloakGoal = hasCloak ? r.F32() : 0.f;
            struct WeaponState { bool powered; float charge; };
            std::vector<WeaponState> weapons(r.U8());
            for (WeaponState &weapon : weapons)
            {
                weapon.powered = r.Bool();
                weapon.charge = r.F32();
            }
            if (!Drones::ReadState(r) || !Crew::ReadState(r) || !Rooms::ReadState(r) || !r.Ok()) return;

            // Snapshots may arrive out of order; only newer ones count.
            if (g_match.havePeerState && (uint16_t)(seq - g_match.peerStateSeq) >= 32768) return;
            g_match.havePeerState = true;
            g_match.peerStateSeq = seq;

            ShipManager *replica = G_->GetShipManager(1);
            if (!replica || !g_match.replicaReady) return;

            replica->ship.hullIntegrity.first = std::min(hull, replica->ship.hullIntegrity.second);

            // The owner's locks first (ion, and the battery's): power is then set the way the owner's was changed.
            ApplyLocks(replica, systems);

            // The battery before the systems: the extra power it gives must be there for them to draw on. Its timer
            // runs behind the owner's like the lock timers, so it goes off when the owner's does.
            BatterySystem *battery = replica->batterySystem;
            if (hasBattery && battery)
            {
                if (batteryOn && !battery->bTurnedOn) battery->SetTurnedOn(true, true);
                else if (!batteryOn && battery->bTurnedOn) battery->SetTurnedOn(false, false);
                if (battery->bTurnedOn) battery->timer.currTime = std::max(0.f, batteryTime - LOCK_TIMER_LAG_S);
            }

            // Damage first, so power never exceeds what the system can hold.
            for (const SystemState &state : systems)
            {
                ShipSystem *system = replica->GetSystem(state.id);
                if (!system) continue;
                int health = std::max(0, std::min(state.health, system->healthState.second));
                if (system->healthState.first != health)
                {
                    system->healthState.first = health;
                    if (state.id != SYS_DRONES && PowerBars(system) > health) SetReplicaPower(replica, system, health);
                }
            }
            for (const SystemState &state : systems)
            {
                // Weapons and drones power follows the weapons and drones, one by one (below, and DuelsDrones.cpp):
                // raising drone power by itself would launch the replica's drones in slot order.
                ShipSystem *system = replica->GetSystem(state.id);
                if (state.id == SYS_WEAPONS || state.id == SYS_DRONES || !system) continue;
                if (!system->bNeedsPower) g_match.subsystemPower[state.id] = state.power;
                if (PowerBars(system) != state.power) SetReplicaPower(replica, system, state.power);
                if (PowerBars(system) != state.power) LogPowerMiss(system, state.power);
            }

            // The cloak after the systems, so its power is there.
            if (hasCloak && replica->cloakSystem) SetReplicaCloak(replica->cloakSystem, cloakOn, cloakTime, cloakGoal);

            if (hasShields && replica->shieldSystem)
            {
                replica->shieldSystem->shields.power.first = std::min(shieldLayers, 16);
                replica->shieldSystem->shields.charger = shieldCharge;
                replica->shieldSystem->shields.power.super.second = superShieldMax;
                replica->shieldSystem->shields.power.super.first = std::min(superShield, superShieldMax);
            }

            if (replica->weaponSystem)
            {
                std::vector<ProjectileFactory*> list = replica->GetWeaponList();
                for (size_t slot = 0; slot < list.size() && slot < weapons.size(); ++slot)
                {
                    ProjectileFactory *weapon = list[slot];
                    if (weapons[slot].powered && !weapon->powered) replica->PowerWeapon(weapon, true, true);
                    else if (!weapons[slot].powered && weapon->powered) replica->DePowerWeapon(weapon, true);
                    weapon->cooldown.first = std::min(weapons[slot].charge, weapon->cooldown.second);
                }
            }

            // Drones after the systems, so the reactor power they need is free.
            Drones::ApplyState(Net::HasClock() ? Net::PeerToLocalTime(sentAt) : WallMs());
            Crew::ApplyState(Net::HasClock() ? Net::PeerToLocalTime(sentAt) : WallMs());
            Rooms::ApplyState();

            // Again, as switching the battery sets its own lock. Between updates the replica counts its lock timers on
            // (ShipSystem::OnLoop), so the lock display runs smoothly.
            ApplyLocks(replica, systems);
            ++g_match.statesApplied;
        }

        // --------------------------------------------------------------------------------------------------------
        // Shots
        // --------------------------------------------------------------------------------------------------------

        // Tells the opponent about a shot of ours at their ship: where it goes and what the defender needs to build the
        // same projectile. A drone's shot starts in their space, at the drone.
        static void SendShot(Projectile *projectile, const WeaponBlueprint *blueprint, int slot, SpaceDrone *drone)
        {
            MatchState &m = g_match;
            int type = blueprint->type;
            double now = WallMs();
            OutShot shot;
            shot.projectile = projectile;
            shot.selfId = projectile->selfId;
            shot.netId = m.nextNetId++;
            shot.slot = slot;
            shot.type = type;
            shot.weapon = blueprint->name;
            shot.spawnMs = now;
            shot.drone = drone != nullptr;
            m.out.push_back(shot);

            double leg = drone ? 0.0 : slot < MAX_SLOTS && m.legEstimate[slot] > 0.0 ? m.legEstimate[slot]
                                                                                        : (type == WEAPON_MISSILES ? 560.0 : 330.0);
            Writer w;
            w.U32(shot.netId);
            w.U8((uint8_t)slot);
            w.Str(shot.weapon);
            w.F32(projectile->target.x);
            w.F32(projectile->target.y);
            w.F64(now);
            w.F32((float)leg);
            // What the defender needs to build the same projectile: a beam's second point (it sweeps from the first),
            // a bomb's Zoltan-shield bypass, a flak shard's look (and whether it is one of the harmless ones).
            w.U8((uint8_t)type);
            if (type == WEAPON_BEAM)
            {
                BeamWeapon *beam = static_cast<BeamWeapon*>(projectile);
                w.F32(beam->target2.x);
                w.F32(beam->target2.y);
            }
            else if (type == WEAPON_BOMB)
            {
                w.U8(static_cast<BombProjectile*>(projectile)->superShieldBypass ? 1 : 0);
            }
            else if (type == WEAPON_BURST)
            {
                uint8_t shard = 0xFF;
                for (size_t i = 0; i < blueprint->miniProjectiles.size() && i < 0xFF; ++i)
                {
                    if (blueprint->miniProjectiles[i].image == projectile->flight_animation.animName)
                    {
                        shard = (uint8_t)i;
                        break;
                    }
                }
                w.U8(shard);
                w.U8(projectile->damage.iDamage == 0 && blueprint->damage.iDamage > 0 ? 1 : 0);
            }
            // Where it comes from: a weapon of our ship, or one of our drones (position in their space, and its aim).
            w.Bool(drone != nullptr);
            if (drone)
            {
                w.F32(projectile->position.x);
                w.F32(projectile->position.y);
                w.F32(drone->aimingAngle);
            }
            Net::Send(MSG_SHOT, w, true);
            ++m.shotsSent;
        }

        static bool CanNetwork(const WeaponBlueprint *blueprint)
        {
            if (Networked(blueprint->type)) return true;
            if (g_match.unsupportedLogged.insert(blueprint->name).second)
            {
                Announce(blueprint->name + " is not networked yet; the opponent won't see it");
            }
            return false;
        }

        void OnOwnProjectile(ProjectileFactory *weapon, Projectile *projectile)
        {
            MatchState &m = g_match;
            if (!Net::IsConnected() || !m.replicaReady || !projectile || projectile->destinationSpace != 1) return;
            ShipManager *ship = G_->GetShipManager(0);
            int slot = WeaponSlot(ship, weapon);
            if (slot < 0 || !weapon->blueprint || !CanNetwork(weapon->blueprint)) return;
            SendShot(projectile, weapon->blueprint, slot, nullptr);
        }

        void OnOwnDroneProjectile(SpaceDrone *drone, Projectile *projectile)
        {
            if (!projectile || !drone || drone->iShipId != 0) return;
            if (projectile->currentSpace == drone->iShipId)
            {
                // A defense drone's shot in our space: it may hit their shots and drones here; they see a copy.
                Drones::OnOwnDroneShotInOwnSpace(drone, projectile);
                return;
            }
            MatchState &m = g_match;
            if (!Net::IsConnected() || !m.replicaReady || projectile->currentSpace != 1 || !drone->weaponBlueprint) return;
            int slot = Drones::SlotOf(G_->GetShipManager(0), drone);
            if (slot < 0 || !CanNetwork(drone->weaponBlueprint)) return;
            SendShot(projectile, drone->weaponBlueprint, slot, drone);
        }

        static void SendResult(InShot &shot, uint8_t outcome, int damage, Pointf point = Pointf(0.f, 0.f))
        {
            double now = WallMs();
            shot.outcome = outcome;
            shot.damage = damage;
            shot.decisionMs = now;
            Writer w;
            w.U32(shot.netId);
            w.U8(outcome);
            w.I8((int8_t)damage);
            if (outcome == OUTCOME_DOWNED)
            {
                w.F32(point.x);
                w.F32(point.y);
            }
            Net::Send(MSG_RESULT, w, true);
            ++g_match.verdictsSent;
            g_match.stateDirty = true;   // the damage should arrive together with the verdict
        }

        // A flak shard's own look; the harmless ones do no damage (as CustomWeapons.cpp builds them).
        static void MakeShard(Projectile *projectile, const WeaponBlueprint *blueprint, int shard, bool fakeShard)
        {
            const WeaponBlueprint::MiniProjectile &mini = blueprint->miniProjectiles[shard < (int)blueprint->miniProjectiles.size() ? shard : 0];
            projectile->flight_animation = G_->GetAnimationControl()->GetAnimation(mini.image);
            projectile->flight_animation.SetCurrentFrame(random32() % std::max(1, projectile->flight_animation.info.numFrames));
            projectile->flight_animation.Stop();
            if (!fakeShard && !mini.fake) return;
            Damage &damage = projectile->damage;
            damage.iDamage = 0;
            damage.iShieldPiercing = 0;
            damage.fireChance = 0;
            damage.breachChance = 0;
            damage.stunChance = 0;
            damage.iIonDamage = 0;
            damage.iSystemDamage = 0;
            damage.iPersDamage = 0;
            damage.bHullBuster = false;
            damage.ownerId = -1;
            damage.selfId = -1;
            damage.bLockdown = false;
            damage.crystalShard = false;
            damage.bFriendlyFire = true;
            damage.iStun = 0;
            projectile->death_animation.fScale = 0.25f;
        }

        static void PlayLaunchSound(const WeaponBlueprint *blueprint)
        {
            if (blueprint->effects.launchSounds.empty()) return;
            G_->GetSoundControl()->PlaySoundMix(blueprint->effects.launchSounds[random32() % blueprint->effects.launchSounds.size()], -1.f, false);
        }

        static void CreateIncomingShot(Reader &r)
        {
            uint32_t netId = r.U32();
            int slot = r.U8();
            std::string weaponName = r.Str();
            Pointf target;
            target.x = r.F32();
            target.y = r.F32();
            double peerSpawn = r.F64();
            double ownLeg = r.F32();
            int type = r.U8();
            Pointf target2 = target;
            bool bombBypass = false;
            int shard = 0xFF;
            bool fakeShard = false;
            if (type == WEAPON_BEAM)
            {
                target2.x = r.F32();
                target2.y = r.F32();
            }
            else if (type == WEAPON_BOMB)
            {
                bombBypass = r.U8() != 0;
            }
            else if (type == WEAPON_BURST)
            {
                shard = r.U8();
                fakeShard = r.U8() != 0;
            }
            bool fromDrone = r.Bool();
            Pointf origin;
            float droneAim = 0.f;
            if (fromDrone)
            {
                origin.x = r.F32();
                origin.y = r.F32();
                droneAim = r.F32();
            }
            if (!r.Ok()) return;
            MatchState &m = g_match;
            ++m.shotsReceived;
            double now = WallMs();

            InShot shot;
            shot.netId = netId;
            shot.type = type;
            shot.weapon = weaponName;
            shot.receivedMs = now;
            shot.spawnMs = Net::HasClock() ? Net::PeerToLocalTime(peerSpawn) : now;
            shot.drone = fromDrone;

            ShipManager *replica = G_->GetShipManager(1);
            ShipManager *own = G_->GetShipManager(0);
            ProjectileFactory *weapon = nullptr;
            const WeaponBlueprint *blueprint = nullptr;
            SpaceDrone *drone = nullptr;
            if (fromDrone)
            {
                // A drone's weapon is a plain blueprint; the drone itself is the replica's (a puppet) in that slot.
                blueprint = G_->GetBlueprints()->GetWeaponBlueprint(weaponName);
                if (blueprint && blueprint->name != weaponName) blueprint = nullptr;
                if (replica && replica->droneSystem && slot < (int)replica->droneSystem->drones.size())
                {
                    Drone *candidate = replica->droneSystem->drones[slot];
                    if (candidate->type == 1 || candidate->type == 5) drone = static_cast<SpaceDrone*>(candidate);
                }
            }
            else if (replica && replica->weaponSystem)
            {
                std::vector<ProjectileFactory*> list = replica->GetWeaponList();
                if (slot < (int)list.size() && list[slot]->blueprint && list[slot]->blueprint->name == weaponName) weapon = list[slot];
                blueprint = weapon ? weapon->blueprint : nullptr;
            }
            if (!blueprint || !own || blueprint->type != type || !Networked(type) || (!fromDrone && !weapon))
            {
                Log("Match: shot %u from %s %s %d cannot be shown (no such %s on the replica)", netId, weaponName.c_str(),
                    fromDrone ? "drone" : "slot", slot, fromDrone ? "drone weapon" : "weapon");
                SendResult(shot, OUTCOME_GONE, 0);
                return;
            }

            Projectile *projectile = nullptr;
            if (fromDrone)
            {
                // A drone fires from its orbit, outside our shields; a shot from inside them would pass them unseen.
                // One claimed from inside (a bug, or a changed game) is moved out onto the combat drones' orbit,
                // 1.15 x the shield ellipse, along the line from the ellipse's centre.
                Globals::Ellipse shields = own->ship.GetBaseEllipse();
                float dx = origin.x - shields.center.x, dy = origin.y - shields.center.y;
                float radius = shields.a > 0.f && shields.b > 0.f
                    ? std::sqrt(dx * dx / (shields.a * shields.a) + dy * dy / (shields.b * shields.b)) : 2.f;
                if (radius < 1.f)
                {
                    if (radius < 0.001f)
                    {
                        dx = 0.f;
                        dy = -shields.b;
                        radius = 1.f;
                    }
                    Pointf moved(shields.center.x + dx * 1.15f / radius, shields.center.y + dy * 1.15f / radius);
                    Log("Match: drone shot %u starts inside our shields at (%.0f, %.0f); moved out to (%.0f, %.0f)", netId,
                        origin.x, origin.y, moved.x, moved.y);
                    origin = moved;
                }
                // As SpaceDrone::GetNextProjectile builds it: in our space from the start, at the drone, owned by the
                // replica (the constructor takes the space as the owner).
                switch (type)
                {
                case WEAPON_LASER:
                case WEAPON_BURST:
                {
                    LaserBlast *laser = new LaserBlast(origin, 0, 0, target);
                    laser->heading = -1.f;
                    laser->OnInit();
                    projectile = laser;
                    break;
                }
                case WEAPON_MISSILES:
                    projectile = new Missile(origin, 0, 0, target, droneAim);
                    break;
                case WEAPON_BEAM:
                    projectile = new BeamWeapon(origin, 0, 0, target, target2, blueprint->length, &own->_targetable, droneAim);
                    break;
                default:
                {
                    BombProjectile *bomb = new BombProjectile(origin, 0, 0, target);
                    bomb->superShieldBypass = bombBypass;
                    projectile = bomb;
                    break;
                }
                }
                projectile->Initialize(*blueprint);
                projectile->ownerId = 1;
                if (type == WEAPON_BURST && !blueprint->miniProjectiles.empty()) MakeShard(projectile, blueprint, shard, fakeShard);
                else if (drone) projectile->flight_animation = drone->weapon_animation;
                G_->GetWorld()->space.AddProjectile(projectile);
                PlayLaunchSound(blueprint);
                // No flight between the spaces to hide the delay in: it goes at once and makes up the one-way latency
                // over its first frames, so the attacker's copy waits at our shields for about that long, not the round
                // trip. A beam keeps its pace; a bomb has no flight.
                shot.releaseAt = 0.0;
                shot.speedUp = 1;
                if (type != WEAPON_BEAM && type != WEAPON_BOMB) shot.catchUpMs = Net::RttMs() * 0.5;
            }
            else
            {
                // Like ProjectileFactory::Update does it (CustomWeapons.cpp), at the replica's weapon mount.
                Point fireLocation = weapon->weaponVisual.GetFireLocation() + weapon->localPosition;
                if (type == WEAPON_MISSILES)
                {
                    if (weapon->currentFiringAngle == 0.f) fireLocation.x += 16;
                    else if (weapon->currentFiringAngle == 270.f) fireLocation.y -= 16;
                }
                Pointf position((float)fireLocation.x, (float)fireLocation.y);
                switch (type)
                {
                case WEAPON_LASER:
                case WEAPON_BURST:
                {
                    LaserBlast *laser = new LaserBlast(position, 1, 0, target);
                    laser->OnInit();
                    projectile = laser;
                    break;
                }
                case WEAPON_MISSILES:
                    projectile = new Missile(position, 1, 0, target, weapon->currentFiringAngle);
                    break;
                case WEAPON_BEAM:
                {
                    BeamWeapon *beam = new BeamWeapon(position, 1, 0, target, target2, blueprint->length, &own->_targetable,
                                                      weapon->currentFiringAngle);
                    beam->SetWeaponAnimation(&weapon->weaponVisual);
                    projectile = beam;
                    break;
                }
                default:
                {
                    BombProjectile *bomb = new BombProjectile(position, 1, 0, target);
                    bomb->superShieldBypass = bombBypass;
                    projectile = bomb;
                    break;
                }
                }
                projectile->entryAngle = weapon->currentEntryAngle;
                projectile->Initialize(*blueprint);
                projectile->heading = weapon->currentFiringAngle;
                if (type == WEAPON_BURST && !blueprint->miniProjectiles.empty()) MakeShard(projectile, blueprint, shard, fakeShard);
                else projectile->flight_animation = weapon->flight_animation;
                G_->GetWorld()->space.AddProjectile(projectile);

                // The weapon fires once per volley: a flak volley's shards arrive together.
                bool sameVolley = type == WEAPON_BURST && slot < MAX_SLOTS && now - m.lastFireMs[slot] < 100.0;
                if (slot < MAX_SLOTS) m.lastFireMs[slot] = now;
                if (!sameVolley)
                {
                    weapon->weaponVisual.StartFire();
                    PlayLaunchSound(blueprint);
                }

                // Timing: our copy should enter our space when theirs entered the replica's space on their screen, less
                // the one-way latency, so the verdict is back when their shot reaches our shields. The flight out of the
                // enemy window runs faster to make up for the time the message took. A beam has no flight: it reaches
                // across at once, and its sweep keeps the weapon's own pace.
                double oneWay = Net::RttMs() * 0.5;
                if (type == WEAPON_BEAM)
                {
                    shot.releaseAt = 0.0;
                    shot.speedUp = 1;
                }
                else
                {
                    shot.releaseAt = shot.spawnMs + ownLeg - oneWay;
                    double available = shot.releaseAt - now;
                    shot.speedUp = available >= NATURAL_OPPONENT_LEG_MS ? 1
                                   : std::min(8, (int)std::ceil(NATURAL_OPPONENT_LEG_MS / std::max(available, 120.0)));
                }
            }
            shot.projectile = projectile;
            shot.selfId = projectile->selfId;
            m.in.push_back(shot);
        }

        static void ApplyResult(Reader &r)
        {
            uint32_t netId = r.U32();
            uint8_t outcome = r.U8();
            int damage = r.I8();
            Pointf point;
            if (outcome == OUTCOME_DOWNED)
            {
                point.x = r.F32();
                point.y = r.F32();
            }
            if (!r.Ok()) return;
            ++g_match.verdictsReceived;
            for (OutShot &shot : g_match.out)
            {
                if (shot.netId != netId) continue;
                if (shot.verdict == PENDING)
                {
                    shot.verdict = outcome;
                    shot.damage = damage;
                    shot.verdictMs = WallMs();
                    shot.downPoint = point;
                }
                return;
            }
            Log("Match: verdict %s for shot %u, which is gone already", OutcomeName(outcome), netId);
        }

        // The attacker's shot ran into something in the attacker's own space: its copy on our screen goes too.
        static void ApplyShotDowned(Reader &r)
        {
            uint32_t netId = r.U32();
            float x = r.F32();
            float y = r.F32();
            if (!r.Ok()) return;
            WorldManager *world = G_->GetWorld();
            for (InShot &shot : g_match.in)
            {
                if (shot.netId != netId) continue;
                if (shot.outcome == PENDING)
                {
                    shot.outcome = OUTCOME_DOWNED;
                    shot.decisionMs = WallMs();
                }
                Projectile *projectile = shot.projectile;
                bool alive = world && projectile &&
                             std::find(world->space.projectiles.begin(), world->space.projectiles.end(), projectile) != world->space.projectiles.end() &&
                             projectile->selfId == shot.selfId;
                if (alive && !projectile->startedDeath)
                {
                    projectile->death_animation.Start(true);
                    projectile->startedDeath = true;
                    projectile->missed = true;
                }
                Log("Match: shot %u ran into something on the attacker's side (%.0f, %.0f)", netId, x, y);
                return;
            }
        }

        int ProjectileUpdates(Projectile *projectile)
        {
            InShot *shot = FindIn(projectile);
            if (!shot) return 1;
            double now = WallMs();
            if (projectile->currentSpace == 1) return shot->speedUp;

            if (shot->transferMs < 0.0) shot->transferMs = now;
            if (!shot->released)
            {
                if (now < shot->releaseAt) return 0;   // waits at its entry point, just outside our view
                shot->released = true;
                shot->releasedMs = now;
            }
            if (shot->catchUpMs > 0.0)
            {
                // Three updates a frame gain two frames of flight each frame (SpeedFactor is 16 per second).
                double frameMs = std::max(1.0, (double)G_->GetCFPS()->GetSpeedFactor() * 62.5);
                shot->catchUpMs -= 2.0 * frameMs;
                return 3;
            }
            return 1;
        }

        // Would this frame's movement decide the shot (cross the shields, or reach the target point)?
        static bool WouldDecide(Projectile *projectile, ShipManager *replica)
        {
            if (projectile->AtTarget()) return true;
            Shields *shields = replica->shieldSystem;
            if (!shields) return false;
            Damage damage = projectile->damage;
            return shields->CollisionTest(projectile->last_position.x, projectile->last_position.y, damage).collision_type == 0 &&
                   shields->CollisionTest(projectile->position.x, projectile->position.y, damage).collision_type != 0;
        }

        // A shot of ours that the defender shot down (or that ran into a drone there) explodes on our screen too.
        static void ExplodeOutShot(OutShot &shot)
        {
            Projectile *projectile = shot.projectile;
            if (!projectile || shot.exploded) return;
            shot.exploded = true;
            if (!projectile->startedDeath)
            {
                projectile->death_animation.Start(true);
                projectile->startedDeath = true;
                projectile->missed = true;
                if (!projectile->hitSolidSound.empty()) G_->GetSoundControl()->PlaySoundMix(projectile->hitSolidSound, -1.f, false);
            }
            Drones::EndVisualShotsNear(projectile->position.x, projectile->position.y, projectile->currentSpace);
        }

        static bool IsShipOrDrone(Collideable *other)
        {
            for (int shipId = 0; shipId < 2; ++shipId)
            {
                ShipManager *ship = G_->GetShipManager(shipId);
                if (ship && other == &ship->_collideable) return true;
            }
            WorldManager *world = G_->GetWorld();
            if (!world) return false;
            for (SpaceDrone *drone : world->space.drones)
            {
                if (other == &drone->_collideable) return true;
            }
            return false;
        }

        bool BeginCollisionCheck(Projectile *projectile, Collideable *other)
        {
            g_forced = nullptr;
            ShipManager *replica = G_->GetShipManager(1);
            if (!replica) return true;
            if (g_match.replicaReady && GetState().aiOff[1])
            {
                // Whose space decides (DuelsDrones.h). Copies of the opponent's drone shots never collide. In their
                // space only our shots at their ship count, by the defender's verdict. In ours, a shot hits another
                // shot only when one of them is our drones' (a defense drone's); everything else there is as in FTL.
                if (Drones::IsVisualShot(projectile)) return false;
                if (projectile->currentSpace == 1)
                {
                    if (other != &replica->_collideable) return false;
                }
                else if (!IsShipOrDrone(other))
                {
                    // Collideable is a Projectile's first base: anything but a ship or a drone is a projectile.
                    Projectile *hit = static_cast<Projectile*>(other);
                    return Drones::IsOwnDroneShot(projectile) || Drones::IsOwnDroneShot(hit);
                }
            }
            if (other != &replica->_collideable) return true;
            OutShot *shot = FindOut(projectile);
            if (!shot) return true;
            if (shot->verdict == OUTCOME_DOWNED)
            {
                ExplodeOutShot(*shot);
                return false;
            }

            if (shot->verdict == PENDING)
            {
                if (!WouldDecide(projectile, replica)) return true;
                // Wait just outside the decision point until the defender's verdict arrives.
                if (shot->holdStartMs < 0.0) shot->holdStartMs = WallMs();
                projectile->position = projectile->last_position;
                return false;
            }
            g_forced = shot;
            return true;
        }

        void EndCollisionCheck()
        {
            g_forced = nullptr;
        }

        // A bomb appears in its target room and goes off after a short delay; the dodge is rolled when it appears
        // (a dodged bomb is set aside with a "MISS"). Our bomb in the replica never rolls: it waits at the moment it
        // would go off until the defender's verdict is here, then goes off or misses as the defender's did.
        bool BeginBombCheck(BombProjectile *bomb, Collideable *other)
        {
            g_forced = nullptr;
            ShipManager *replica = G_->GetShipManager(1);
            if (!replica || other != &replica->_collideable || bomb->currentSpace != 1) return true;
            OutShot *shot = FindOut(bomb);
            if (!shot) return true;
            if (bomb->explosiveDelay > 0.f || bomb->startedDeath || bomb->bMissed) return true;

            if (shot->verdict == PENDING)
            {
                if (shot->holdStartMs < 0.0) shot->holdStartMs = WallMs();
                return false;
            }
            if (shot->verdict == OUTCOME_MISS || shot->verdict == OUTCOME_GONE)
            {
                bomb->bMissed = true;
                replica->damMessages.push_back(new DamageMessage(1.f, bomb->position, DamageMessage::MISS));
            }
            g_forced = shot;
            return true;
        }

        bool ForcedDodge(ShipManager *ship, bool &dodged)
        {
            if (!ship || ship->iShipId != 1) return false;
            OutShot *shot = FindOut(CustomDamageManager::currentProjectile);
            if (!shot || shot->type != WEAPON_BOMB) return false;
            dodged = false;   // decided when it goes off (BeginBombCheck)
            return true;
        }

        void ObserveDodge(ShipManager *ship, bool dodged)
        {
            if (!ship || ship->iShipId != 0 || !dodged) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (shot && shot->type == WEAPON_BOMB && shot->outcome == PENDING) SendResult(*shot, OUTCOME_MISS, 0);
        }

        // Our beam sweeping the replica: the defender's game does the damage (and the state sync shows it), ours only
        // draws it. It still runs with no damage, so the beam looks and sounds as it does.
        void MuteBeamDamage(ShipManager *ship, Damage &damage)
        {
            if (!ship || ship->iShipId != 1) return;
            OutShot *shot = FindOut(CustomDamageManager::currentProjectile);
            if (!shot || shot->type != WEAPON_BEAM) return;
            damage.iDamage = 0;
            damage.iShieldPiercing = 0;
            damage.fireChance = 0;
            damage.breachChance = 0;
            damage.stunChance = 0;
            damage.iIonDamage = 0;
            damage.iSystemDamage = 0;
            damage.iPersDamage = 0;
            damage.bHullBuster = false;
            damage.bLockdown = false;
            damage.crystalShard = false;
            damage.iStun = 0;
        }

        // The opponent's beam sweeping our ship: its verdict (for the trace) is sent when it is over.
        void ObserveBeam(ShipManager *ship, bool hit, int hullBefore)
        {
            if (!ship || ship->iShipId != 0) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (!shot || shot->type != WEAPON_BEAM) return;
            shot->beamHit = shot->beamHit || hit;
            shot->damage += std::max(0, hullBefore - ship->ship.hullIntegrity.first);
        }

        static bool ForcedApplies(ShipManager *ship)
        {
            return g_forced && ship && ship->iShipId == 1 && CustomDamageManager::currentProjectile == g_forced->projectile;
        }

        bool ForcedShieldResponse(ShipManager *ship, Pointf start, Pointf finish, const Damage &damage,
                                  CollisionResponse &response)
        {
            if (!ForcedApplies(ship)) return false;
            response.collision_type = 0;
            response.point = Pointf(-2147483648.f, -2147483648.f);
            response.damage = 0;
            response.superDamage = 0;

            Shields *shields = ship->shieldSystem;
            if (!shields) return true;
            bool crossing = shields->CollisionTest(start.x, start.y, damage).collision_type == 0 &&
                            shields->CollisionTest(finish.x, finish.y, damage).collision_type != 0;
            if (!crossing) return true;

            switch (g_forced->verdict)
            {
            case OUTCOME_MISS:
            case OUTCOME_GONE:
                ship->damMessages.push_back(new DamageMessage(1.f, finish, DamageMessage::MISS));
                response.collision_type = 3;
                break;
            case OUTCOME_SHIELD:
                // Let the replica's shields show the hit; the state update brings the exact layers anyway.
                response = shields->CollisionReal(finish.x, finish.y, damage, false);
                if (response.collision_type != 2)
                {
                    response.collision_type = 2;
                    response.point = finish;
                    response.damage = damage.iDamage;
                }
                break;
            default:
                break;   // a hit goes through the shields; the hull decides below
            }
            return true;
        }

        bool ForcedDamageArea(ShipManager *ship, Pointf location, bool &hit)
        {
            if (!ForcedApplies(ship)) return false;
            switch (g_forced->verdict)
            {
            case OUTCOME_HIT:
                if (g_forced->damage > 0) ship->damMessages.push_back(new DamageMessage(1.f, g_forced->damage, location, false));
                hit = true;
                break;
            case OUTCOME_SHIELD:
                hit = true;   // their shields took it; ours were a moment out of date
                break;
            default:
                if (!ship->bJumping) ship->damMessages.push_back(new DamageMessage(1.f, location, DamageMessage::MISS));
                hit = false;
                break;
            }
            return true;
        }

        void ObserveShield(ShipManager *ship, const CollisionResponse &response)
        {
            if (!ship || ship->iShipId != 0) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (!shot || shot->outcome != PENDING) return;
            if (shot->type == WEAPON_BEAM)
            {
                // A beam touches the shields every frame of its sweep; its verdict is sent when it is over.
                if (response.collision_type == 2) shot->beamTouched = true;
                return;
            }
            if (response.collision_type == 3) SendResult(*shot, OUTCOME_MISS, 0);
            else if (response.collision_type == 2) SendResult(*shot, OUTCOME_SHIELD, 0);
        }

        void ObserveDamageArea(ShipManager *ship, bool hit, int hullBefore)
        {
            if (!ship || ship->iShipId != 0) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (!shot || shot->outcome != PENDING) return;
            SendResult(*shot, hit ? OUTCOME_HIT : OUTCOME_MISS, hit ? std::max(0, hullBefore - ship->ship.hullIntegrity.first) : 0);
        }

        // Shots whose projectile is gone are logged and forgotten; transfers feed the timing estimates.
        // The duel ends (leave, disconnect, quit) with shots still flying: they go into duels_shots.csv as they stand.
        static void FlushShotLog()
        {
            MatchState &m = g_match;
            for (const OutShot &shot : m.out)
            {
                LogShot("out", shot.netId, shot.weapon, shot.spawnMs, -1.0, shot.transferMs, -1.0, shot.holdStartMs, -1.0,
                        shot.verdictMs, shot.verdict, shot.damage, 1);
            }
            for (const InShot &shot : m.in)
            {
                LogShot("in", shot.netId, shot.weapon, shot.spawnMs, shot.receivedMs, shot.transferMs, shot.releasedMs, -1.0,
                        shot.decisionMs, -1.0, shot.outcome, shot.damage, shot.speedUp);
            }
            m.out.clear();
            m.in.clear();
        }

        static void TrackShots(double now)
        {
            MatchState &m = g_match;
            WorldManager *world = G_->GetWorld();
            std::set<Projectile*> live;
            if (world)
            {
                for (Projectile *projectile : world->space.projectiles) live.insert(projectile);
            }

            for (size_t i = 0; i < m.out.size();)
            {
                OutShot &shot = m.out[i];
                bool alive = live.count(shot.projectile) && shot.projectile->selfId == shot.selfId;
                if (alive)
                {
                    Projectile *projectile = shot.projectile;
                    if (shot.transferMs < 0.0 && projectile->currentSpace == 1)
                    {
                        shot.transferMs = now;
                        if (!shot.drone && shot.slot < MAX_SLOTS)
                        {
                            double leg = now - shot.spawnMs;
                            double &estimate = m.legEstimate[shot.slot];
                            estimate = estimate > 0.0 ? estimate * 0.7 + leg * 0.3 : leg;
                        }
                    }
                    // It ran into something in our own space before it left (one of their drones): that is ours to
                    // decide, and the defender's copy goes too.
                    if (shot.verdict == PENDING && projectile->startedDeath && projectile->currentSpace == 0)
                    {
                        shot.verdict = OUTCOME_DOWNED;
                        shot.verdictMs = now;
                        shot.exploded = true;
                        Writer w;
                        w.U32(shot.netId);
                        w.F32(projectile->position.x);
                        w.F32(projectile->position.y);
                        Net::Send(MSG_SHOT_DOWNED, w, true);
                    }
                    // Shot down on the defender's screen: it explodes where that happened. It is about there now, as
                    // the defender's copy flies ahead of ours by the latency; at the latest where it stops coming closer.
                    if (shot.verdict == OUTCOME_DOWNED && !shot.exploded)
                    {
                        float dx = projectile->position.x - shot.downPoint.x, dy = projectile->position.y - shot.downPoint.y;
                        float distance = std::sqrt(dx * dx + dy * dy);
                        bool passed = distance > shot.downDistance + 0.5f;
                        shot.downDistance = std::min(shot.downDistance, distance);
                        if (projectile->currentSpace == 1 &&
                            (distance < 15.f || passed || shot.holdStartMs >= 0.0 || now - shot.verdictMs > 500.0))
                        {
                            ExplodeOutShot(shot);
                        }
                    }
                    if (shot.verdict == PENDING && shot.holdStartMs >= 0.0 && now - shot.holdStartMs > HOLD_TIMEOUT_MS)
                    {
                        shot.verdict = OUTCOME_MISS;
                        shot.timedOut = true;
                        shot.verdictMs = now;
                        ++m.holdTimeouts;
                        Log("Match: no verdict for shot %u after %.0f ms; shown as a miss", shot.netId, HOLD_TIMEOUT_MS);
                    }
                    ++i;
                    continue;
                }
                // A beam's verdict comes when the defender's sweep is over, usually after ours: wait a little for it.
                if (shot.type == WEAPON_BEAM && shot.verdict == PENDING)
                {
                    shot.projectile = nullptr;
                    if (shot.goneMs < 0.0) shot.goneMs = now;
                    if (now - shot.goneMs < BEAM_VERDICT_WAIT_MS)
                    {
                        ++i;
                        continue;
                    }
                }
                LogShot("out", shot.netId, shot.weapon, shot.spawnMs, -1.0, shot.transferMs, -1.0, shot.holdStartMs, -1.0,
                        shot.verdictMs, shot.verdict, shot.damage, 1);
                m.out.erase(m.out.begin() + i);
            }

            for (size_t i = 0; i < m.in.size();)
            {
                InShot &shot = m.in[i];
                bool alive = live.count(shot.projectile) && shot.projectile->selfId == shot.selfId;
                if (alive)
                {
                    // It exploded in our space before our shields: one of our defense drones shot it down, or it ran
                    // into a drone. (Hits on our ship decide it first; beams and bombs can't be shot down.)
                    Projectile *projectile = shot.projectile;
                    if (shot.outcome == PENDING && projectile->startedDeath && projectile->currentSpace == 0 &&
                        shot.type != WEAPON_BEAM && shot.type != WEAPON_BOMB)
                    {
                        SendResult(shot, OUTCOME_DOWNED, 0, projectile->position);
                    }
                    ++i;
                    continue;
                }
                if (shot.outcome == PENDING && shot.type == WEAPON_BEAM)
                {
                    // (DamageBeam returns false even when it did damage.)
                    bool hit = shot.beamHit || shot.damage > 0;
                    SendResult(shot, hit ? OUTCOME_HIT : shot.beamTouched ? OUTCOME_SHIELD : OUTCOME_MISS, shot.damage);
                }
                else if (shot.outcome == PENDING)
                {
                    SendResult(shot, OUTCOME_GONE, 0);
                }
                LogShot("in", shot.netId, shot.weapon, shot.spawnMs, shot.receivedMs, shot.transferMs, shot.releasedMs, -1.0,
                        shot.decisionMs, -1.0, shot.outcome, shot.damage, shot.speedUp);
                m.in.erase(m.in.begin() + i);
            }
        }

        // Everything the state sync carries, as one comparable line: hull, shields, systems (id:power/health, and
        // Ln while locked), weapons, drones.
        static std::string Signature(ShipManager *ship)
        {
            std::ostringstream out;
            out << ship->ship.hullIntegrity.first << ',' << (ship->shieldSystem ? ship->shieldSystem->shields.power.first : -1);
            if (ship->shieldSystem && ship->shieldSystem->shields.power.super.first > 0) out << '+' << ship->shieldSystem->shields.power.super.first;
            out << ',';
            for (ShipSystem *system : ship->vSystemList)
            {
                out << system->iSystemType << ':' << PowerBars(system) << '/' << system->healthState.first;
                if (system->iLockCount != 0) out << 'L' << system->iLockCount;   // ion lock; -1 = battery running
                out << ' ';
            }
            out << ',';
            if (ship->weaponSystem)
            {
                for (ProjectileFactory *weapon : ship->GetWeaponList()) out << (weapon->powered ? '1' : '0');
            }
            out << ',' << Drones::Signature(ship);
            out << ',' << Crew::Signature(ship) << ',' << Crew::RoomSignature(ship) << ',' << Rooms::Signature(ship)
                << ',' << Crew::AnimationSignature(ship) << ',' << Bays::Signature(ship);
            out << ',' << (!ship->cloakSystem ? "-" : ship->cloakSystem->bTurnedOn ? "on" : "off");
            return out.str();
        }

        // duels_sync.csv (with "trace on"): every change of our ship and of the replica, to check the replica
        // follows its owner within an update interval (tools/analyze-duel.py).
        static void TraceSync(double now)
        {
            MatchState &m = g_match;
            if (!GetState().trace || !m.replicaReady) return;
            for (int shipId = 0; shipId < 2; ++shipId)
            {
                ShipManager *ship = G_->GetShipManager(shipId);
                if (!ship) continue;
                std::string signature = Signature(ship);
                if (signature == m.lastSignature[shipId]) continue;
                m.lastSignature[shipId] = signature;
                if (!m.syncCsv.IsOpen()) m.syncCsv.Open("duels_sync.csv", "wall_ms,clock_offset_ms,ship,hull,shields,systems,weapons,drones,crew,crew_rooms,rooms,crew_anim,bays,cloak");
                Row row;
                row << now << Net::LocalToPeerTime(0.0) << (shipId == 0 ? "own" : "replica") << signature;
                m.syncCsv.WriteRow(row.str());
            }
        }

        // --------------------------------------------------------------------------------------------------------
        // Session events
        // --------------------------------------------------------------------------------------------------------

        // ---------------------------------------------------------------------------------------------------------
        // Crew experience
        // ---------------------------------------------------------------------------------------------------------

        // Main skills mastered in about two of a match's five fights (docs/design/weapon-bays.md): FTL needs 26
        // dodges for piloting, 100 absorbed hits for shields, 116 volleys for weapons (humans).
        static const float XP_DEFAULT = 3.f, XP_MIN = 1.f, XP_MAX = 10.f;
        static float g_xpSetting = XP_DEFAULT;   // ours: counts when we host
        static float g_xpMatch = XP_DEFAULT;     // this duel's: the host's
        static float g_xpCarry = 0.f;            // what is left of a fraction
        static uint32_t g_xpGains = 0, g_xpCounted = 0;

        static std::string XpText(float factor)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "x%g", factor);
            return text;
        }

        static void SendSettings()
        {
            Writer w;
            w.F32(g_xpMatch);
            Net::Send(MSG_SETTINGS, w, true);
        }

        static void ApplySettings(Reader &r)
        {
            float xp = r.F32();
            if (!r.Ok() || !(xp >= XP_MIN && xp <= XP_MAX)) return;
            if (xp != g_xpMatch) Announce("crew experience " + XpText(xp) + " (the host's setting)");
            g_xpMatch = xp;
        }

        bool SetCrewXp(float factor, std::string &message)
        {
            if (!(factor >= XP_MIN && factor <= XP_MAX))
            {
                message = "usage: xp <factor from 1 to 10> (1 is FTL's own pace)";
                return false;
            }
            if (Net::IsConnected() && !Net::IsHost())
            {
                message = "the host decides the crew experience: " + XpText(g_xpMatch);
                return false;
            }
            g_xpSetting = factor;
            g_xpMatch = factor;
            if (Net::IsConnected()) SendSettings();
            message = "crew experience " + XpText(factor) + (Net::IsConnected() ? " for this duel" : " when you host");
            return true;
        }

        std::string CrewXpStatus()
        {
            std::string text = "crew experience " + XpText(Net::IsConnected() ? g_xpMatch : g_xpSetting);
            if (Net::IsConnected() && !Net::IsHost()) text += " (the host's setting)";
            return text;
        }

        int SkillGains(const CrewMember *crew)
        {
            if (!crew || crew->iShipId != 0 || !Net::IsConnected()) return 1;
            float gain = g_xpMatch + g_xpCarry;
            int whole = std::max(0, (int)gain);
            g_xpCarry = gain - (float)whole;
            ++g_xpGains;
            g_xpCounted += (uint32_t)whole;
            return whole;
        }

        class Listener : public Net::Listener
        {
        public:
            void OnConnected() override
            {
                ResetMatch();
                Announce("connected to " + Net::PeerName() + (Net::IsHost() ? " (you host)" : ""));
                // The host's settings count for both; the guest has the default until they come.
                g_xpMatch = Net::IsHost() ? g_xpSetting : XP_DEFAULT;
                g_xpCarry = 0.f;
                if (Net::IsHost())
                {
                    SendSettings();
                    Announce("crew experience " + XpText(g_xpMatch) + " (your setting, as the host)");
                }
                // Test commands can change ships, so both players see a debug duel for what it is.
                bool ours = GetState().debug, theirs = Net::PeerDebug();
                if (ours || theirs)
                {
                    Announce(std::string("DEBUG DUEL: test commands are on (") + (ours ? "yours on" : "yours off") + ", " +
                             Net::PeerName() + "'s " + (theirs ? "on" : "off") + ")");
                }
            }

            void OnNotice(const std::string &text) override
            {
                Announce(text);
            }

            void OnDisconnected(const std::string &reason, bool opponentGone) override
            {
                // A fight still going on when the other player is gone is won by the player still here (rules,
                // section 3). Rejoining a running match comes with the match flow.
                ShipManager *own = G_->GetShipManager(0);
                ShipManager *replica = G_->GetShipManager(1);
                bool fighting = g_match.replicaReady && own && replica && own->ship.hullIntegrity.first > 0 &&
                                replica->ship.hullIntegrity.first > 0;
                if (fighting && opponentGone)
                {
                    Log("Match: fight won, the opponent is gone (%s)", reason.c_str());
                    Announce(reason + " - you win this fight");
                }
                else
                {
                    Announce("disconnected: " + reason);
                }
                FlushShotLog();
                ResetMatch();
            }

            void OnMessage(uint8_t type, Reader &reader) override
            {
                switch (type)
                {
                case MSG_CHAT:
                    Announce(Net::PeerName() + ": " + reader.Str());
                    break;
                case MSG_SETTINGS:
                    if (!Net::IsHost()) ApplySettings(reader);
                    break;
                case MSG_LOADOUT:
                    ApplyLoadout(reader);
                    break;
                case MSG_READY:
                    g_match.peerReady = true;
                    Log("Match: the opponent has built our ship");
                    break;
                case MSG_STATE:
                    ApplyState(reader);
                    break;
                case MSG_SHOT:
                    CreateIncomingShot(reader);
                    break;
                case MSG_RESULT:
                    ApplyResult(reader);
                    break;
                case MSG_SHOT_DOWNED:
                    ApplyShotDowned(reader);
                    break;
                case Drones::MSG_DRONE_HIT:
                case Drones::MSG_DRONE_SHOT:
                    Drones::OnMessage(type, reader);
                    break;
                case Crew::MSG_CREW_ROSTER:
                    Crew::ApplyRoster(reader);
                    break;
                case MSG_DEFEAT:
                    Announce("the opponent's ship is destroyed - you win this round");
                    break;
                default:
                    Log("Match: unknown message %u", (unsigned)type);
                    break;
                }
            }
        };

        static Listener g_listener;

        void Init()
        {
            if (g_match.initialised) return;
            g_match.initialised = true;
            ResetMatch();
            Net::SetListener(&g_listener);
            Net::SetIdentity(g_match.playerName, VERSION, BUILD_IDENTIFIER_HASH);
        }

        void OnFrame(double now)
        {
            Init();
            Net::Update(now);
            // The enemy window fits and mirrors the opponent's ship while it is a duel replica.
            View::SetDuelOpponent(g_match.replicaReady && G_->GetShipManager(1) != nullptr);
            if (!Net::IsConnected()) return;

            MatchState &m = g_match;
            if (InGame())
            {
                if (!m.loadoutSent) SendLoadout();
                else if (Armament(G_->GetShipManager(0)) != m.sentArmament)
                {
                    Log("Match: our weapons or drones changed their slots; the loadout goes again");
                    SendLoadout();
                }
                if (m.stateDirty || now - m.lastStateSent >= STATE_INTERVAL_MS) SendState(now);
                ShipManager *ship = G_->GetShipManager(0);
                // No escaping a duel: the FTL drive never finishes charging while the opponent is here.
                if (ship && m.replicaReady) ship->jump_timer.first = 0.f;
                if (ship && ship->ship.hullIntegrity.first <= 0 && !m.defeatSent)
                {
                    m.defeatSent = true;
                    Net::Send(MSG_DEFEAT, Writer(), true);
                    Announce("your ship is destroyed - the opponent wins this round");
                }
                TraceSync(now);
            }
            TrackShots(now);
        }

        void SetPlayerName(const std::string &name)
        {
            Init();
            g_match.playerName = name;
            Net::SetIdentity(name, VERSION, BUILD_IDENTIFIER_HASH);
        }

        bool Host(uint16_t port, bool loopbackOnly, std::string &message)
        {
            Init();
            ResetMatch();
            return Net::Host(port, loopbackOnly, message);
        }

        bool Join(const std::string &host, uint16_t port, std::string &message)
        {
            Init();
            ResetMatch();
            return Net::Join(host, port, message);
        }

        bool HostRelay(const std::string &server, uint16_t port, std::string &message)
        {
            Init();
            ResetMatch();
            return Net::HostRelay(server, port, message);
        }

        bool JoinRelay(const std::string &server, uint16_t port, const std::string &code, std::string &message)
        {
            Init();
            ResetMatch();
            return Net::JoinRelay(server, port, code, message);
        }

        void Leave()
        {
            FlushShotLog();
            Net::Leave("left the duel");
            ResetMatch();
        }

        void SetDebug(bool debug)
        {
            Net::SetDebugFlag(debug);
        }

        bool Say(const std::string &text)
        {
            Writer w;
            w.Str(text);
            return Net::Send(MSG_CHAT, w, true);
        }

        std::string Status()
        {
            const MatchState &m = g_match;
            std::ostringstream out;
            out << Net::Status() << " | match: opponent " << (m.opponentShip.empty() ? "-" : m.opponentShip)
                << (m.replicaReady ? " built" : "") << (m.peerReady ? ", ours built there" : "")
                << ", states applied " << m.statesApplied << ", shots out " << m.shotsSent << " in " << m.shotsReceived
                << ", verdicts sent " << m.verdictsSent << " received " << m.verdictsReceived << ", hold timeouts "
                << m.holdTimeouts << ", replica's last hull point kept " << m.hullKept << ", " << Drones::Status() << ", " << Crew::Status() << ", " << Rooms::Status()
                << ", " << Bays::Status() << ", " << CrewXpStatus() << " (skill gains " << g_xpGains << " counted "
                << g_xpCounted << ")";
            return out.str();
        }
    }
}
